#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/mexc/catalogue.hpp>
#include <raikiri/venues/mexc/envelope.hpp>
#include <raikiri/venues/mexc/errors.hpp>
#include <raikiri/venues/mexc/map.hpp>
#include <raikiri/venues/mexc/rest.hpp>
#include <raikiri/venues/mexc/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::mexc::internal {

namespace {

class MexcInstruments final : public exec::InstrumentSource {
public:
    explicit MexcInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}},
          spot_rest_{state_->io, net::RestConfig{.host = std::string{private_rest_host()}, .port = "443"}} {
        rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
        spot_rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
    }

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        switch (market) {
        case MarketType::Perp:
            return load_perp();
        case MarketType::Spot:
            return load_spot();
        case MarketType::Future:
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "mexc lists no dated futures")};
        case MarketType::Option:
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "mexc lists no options")};
        }
        return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "mexc: unknown market")};
    }

private:
    std::expected<std::vector<Instrument>, exec::VenueError> load_perp() {
        rest_.start();

        const auto response = await_response(*state_, rest_, state_->builder.contract_detail(), 20'000);
        if (!response.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        const auto envelope = parse_rest_envelope(response->body);
        if (!envelope.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(envelope.error()))};
        }
        if (!envelope->success || envelope->code != k_ok) {
            return std::unexpected{exec::VenueError::make(classify(envelope->code), envelope->code, envelope->msg)};
        }

        static thread_local const auto storage_owner = std::make_unique<InstrumentBuffer>();

        auto& storage = *storage_owner;
        const auto wire = parse_instruments(response->body, storage);
        if (!wire.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        // before interning, so the schedule is in the instrument from the first read
        const auto schedule = load_schedule();
        return intern(*wire, [&](const WireInstrument& row) {
            auto instrument = map_instrument(row, MarketType::Perp);
            if (!instrument.has_value()) {
                return instrument;
            }
            if (auto* perp = std::get_if<PerpInstrument>(&*instrument); perp != nullptr) {
                if (const auto found = schedule.find(std::string{row.symbol}); found != schedule.end()) {
                    perp->funding_interval_s = found->second.interval_s;
                    perp->funding_cap = found->second.cap;
                    perp->funding_floor = found->second.floor;
                }
            }
            return instrument;
        });
    }

    struct Schedule {
        std::uint32_t interval_s{0};
        FixedE9 cap{};
        FixedE9 floor{};
    };

    // a failed read leaves every interval 0; keys own their strings
    // since the views die with the response body
    std::unordered_map<std::string, Schedule> load_schedule() {
        std::unordered_map<std::string, Schedule> out;
        const auto funding = await_response(*state_, rest_, state_->builder.funding_rates(), 20'000);
        if (!funding.has_value() || funding->transport_failed()) {
            RLOG_WARN("mexc: no funding schedule — every interval reads unstated");
            return out;
        }
        static thread_local const auto buffer_owner = std::make_unique<FundingRateBuffer>();
        const auto rows = parse_funding_rates(funding->body, *buffer_owner);
        if (!rows.has_value()) {
            RLOG_WARN("mexc: funding schedule unreadable ({}) — every interval reads unstated",
                      to_string(rows.error()));
            return out;
        }
        out.reserve(rows->size());
        for (const auto& row : *rows) {
            if (row.collect_cycle_h <= 0) {
                continue;
            }
            Schedule entry{};
            entry.interval_s = static_cast<std::uint32_t>(row.collect_cycle_h) * 3600U;
            // an unparsable cap stays undef
            if (const auto cap = parse_decimal_e9(row.max_rate); cap.has_value()) {
                entry.cap = *cap;
            }
            if (const auto floor = parse_decimal_e9(row.min_rate); floor.has_value()) {
                entry.floor = *floor;
            }
            out.emplace(std::string{row.symbol}, entry);
        }
        RLOG_INFO("mexc: settlement schedule stated for {} contracts", out.size());
        return out;
    }

    std::expected<std::vector<Instrument>, exec::VenueError> load_spot() {
        spot_rest_.start();

        const auto response = await_response(*state_, spot_rest_, state_->builder.exchange_info(), 20'000);
        if (!response.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected{rest_failure(response->body, MarketType::Spot)};
        }

        static thread_local const auto storage_owner = std::make_unique<SpotSymbolBuffer>();

        auto& storage = *storage_owner;
        const auto wire = parse_spot_symbols(response->body, storage);
        if (!wire.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        return intern(*wire, map_spot_instrument);
    }

    template <typename Row, typename Map> std::vector<Instrument> intern(std::span<const Row> rows, Map&& map) {
        std::vector<Instrument> loaded;
        loaded.reserve(rows.size());
        state_->instruments.reserve(state_->instruments.size() + rows.size());
        for (const auto& row : rows) {
            auto instrument = map(row);
            if (!instrument.has_value()) {
                continue; // not enabled, or a scale finer than FixedE9 models
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                continue; // duplicate or table full: reported by count, not by throw
            }
            loaded.push_back(*state_->instruments.find(*id));
        }
        return loaded;
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
    net::RestClient spot_rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<MexcInstruments>(std::move(state));
}

} // namespace raikiri::venues::mexc::internal
