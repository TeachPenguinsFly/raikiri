#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/binance/catalogue.hpp>
#include <raikiri/venues/binance/endpoints.hpp>
#include <raikiri/venues/binance/envelope.hpp>
#include <raikiri/venues/binance/errors.hpp>
#include <raikiri/venues/binance/map.hpp>
#include <raikiri/venues/binance/rest.hpp>
#include <raikiri/venues/binance/ws.hpp>

#include <algorithm>
#include <charconv>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::binance::internal {

namespace {

void sync_clock(VenueState& state, net::RestClient& rest) {
    const auto sent = state.clock->wall();
    const auto response = await_response(state, rest, state.builder.server_time(), 5'000);
    if (!response.has_value() || response->transport_failed()) {
        return;
    }
    const auto venue_ms = parse_server_time(response->body);
    if (!venue_ms.has_value()) {
        return;
    }
    const auto received = state.clock->wall();
    const auto midpoint = sent.ns + (received.ns - sent.ns) / 2;
    state.clock_offset_ns = static_cast<std::int64_t>(*venue_ms * 1'000'000ULL) - static_cast<std::int64_t>(midpoint);
}

class BinanceInstruments final : public exec::InstrumentSource {
public:
    explicit BinanceInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{rest_host(state_->testnet, state_->market)}, .port = "443"}} {
        // no next tick here, so a refusal stops the gateway from starting
        meter_market_data(*state_, rest_);
    }

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        sync_clock(*state_, rest_);

        const auto response = await_response(*state_, rest_, state_->builder.instruments(), 20'000);
        if (!response.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (const auto err = parse_rest_error(response->body); err.has_value() && err->code != k_ok) {
            return std::unexpected{exec::VenueError::make(classify(err->code), err->code, err->msg)};
        }

        static thread_local const auto storage_owner = std::make_unique<InstrumentBuffer>();

        auto& storage = *storage_owner;
        const auto wire = parse_exchange_info(response->body, storage);
        if (!wire.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }

        // kept alive because the rows hold views into it; a failure leaves every interval 0
        struct Schedule {
            std::uint32_t interval_s{0};
            FixedE9 cap{};
            FixedE9 floor{};
        };

        std::unordered_map<std::string_view, Schedule> schedule;
        const bool wants_schedule = market == MarketType::Perp;
        const auto funding = wants_schedule ? await_response(*state_, rest_, state_->builder.funding_info(), 20'000)
                                            : std::nullopt;
        if (wants_schedule && (!funding.has_value() || funding->transport_failed())) {
            RLOG_WARN("binance: no funding schedule — every interval reads unstated");
        } else if (funding.has_value()) {
            static thread_local const auto intervals_owner = std::make_unique<FundingInfoBuffer>();
            auto& intervals = *intervals_owner;
            const auto rows = parse_funding_info(funding->body, intervals);
            if (!rows.has_value()) {
                RLOG_WARN("binance: funding schedule unreadable ({}) — every interval reads "
                          "unstated",
                          to_string(rows.error()));
            } else {
                schedule.reserve(rows->size());
                for (const auto& row : *rows) {
                    Schedule entry{};
                    entry.interval_s = row.interval_hours * 3600U;
                    // an unparsable cap stays undef
                    if (const auto cap = parse_decimal_e9(row.cap); cap.has_value()) {
                        entry.cap = *cap;
                    }
                    if (const auto floor = parse_decimal_e9(row.floor); floor.has_value()) {
                        entry.floor = *floor;
                    }
                    if (entry.interval_s != 0 || !entry.cap.is_undef()) {
                        schedule.emplace(row.symbol, entry);
                    }
                }
            }
        }

        std::vector<Instrument> loaded;
        loaded.reserve(wire->size());
        state_->instruments.reserve(wire->size());
        for (const auto& row : *wire) {
            auto entry = row;
            const auto found = schedule.find(entry.symbol);
            if (found != schedule.end()) {
                entry.funding_interval_s = found->second.interval_s;
            }
            auto instrument = map_instrument(entry, market);
            if (!instrument.has_value()) {
                continue; // not trading, not this market, or missing a filter
            }
            if (found != schedule.end()) {
                if (auto* perp = std::get_if<PerpInstrument>(&*instrument); perp != nullptr) {
                    perp->funding_cap = found->second.cap;
                    perp->funding_floor = found->second.floor;
                }
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                continue; // duplicate or table full: reported by count, not by throw
            }
            loaded.push_back(*state_->instruments.find(*id));
        }
        for (auto& [packed, book] : state_->books) {
            book.readmit();
        }
        return loaded;
    }

private:
    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<BinanceInstruments>(std::move(state));
}

} // namespace raikiri::venues::binance::internal
