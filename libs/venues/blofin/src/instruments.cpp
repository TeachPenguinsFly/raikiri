#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/blofin/catalogue.hpp>
#include <raikiri/venues/blofin/endpoints.hpp>
#include <raikiri/venues/blofin/envelope.hpp>
#include <raikiri/venues/blofin/map.hpp>
#include <raikiri/venues/blofin/market_data.hpp>

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::blofin::internal {

namespace {

constexpr std::uint32_t k_timeout_ms = 10'000;

class BlofinInstruments final : public exec::InstrumentSource {
public:
    explicit BlofinInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, rest_{state_->io,
                                          net::RestConfig{.host = std::string{ep::host(state_->testnet)},
                                                          .port = "443",
                                                          .pool_size = 1,
                                                          .timeout_ms = k_timeout_ms}} {
        rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
    }

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        const auto catalogue = get(ep::k_instruments);
        if (!catalogue.has_value()) {
            return std::unexpected{catalogue.error()};
        }
        static thread_local const auto rows_owner = std::make_unique<InstrumentBuffer>();
        const auto rows = parse_instruments(*catalogue, *rows_owner);
        if (!rows.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }

        // a failed read leaves every period at 0, which a carry board must refuse
        std::unordered_map<std::string_view, const WireFundingRow*> schedule;
        static thread_local const auto funding_owner = std::make_unique<FundingBuffer>();
        std::string funding_body;
        if (auto body = get(ep::k_funding_rate); body.has_value()) {
            funding_body = std::move(*body);
            if (const auto table = parse_funding(funding_body, *funding_owner); table.has_value()) {
                for (const auto& row : *table) {
                    schedule.emplace(row.inst_id, &row);
                }
            }
        } else if (has_logger()) {
            RLOG_WARN("blofin: funding schedule unread ({}) — every period reads unstated",
                      body.error().message.view());
        }

        std::vector<Instrument> loaded;
        loaded.reserve(rows->size());
        state_->instruments.reserve(rows->size());
        std::size_t inverse = 0;
        std::map<std::uint32_t, std::size_t> periods;
        for (const auto& row : *rows) {
            auto instrument = map_instrument(row, market);
            if (!instrument.has_value()) {
                continue;
            }
            auto& perp = std::get<PerpInstrument>(*instrument);
            if (const auto found = schedule.find(row.inst_id); found != schedule.end()) {
                apply_schedule(perp, *found->second);
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                continue;
            }
            inverse += perp.base.kind == ContractKind::Inverse ? 1 : 0;
            ++periods[perp.funding_interval_s];
            loaded.push_back(*state_->instruments.find(*id));
        }
        if (has_logger()) {
            std::string spread;
            for (const auto& [seconds, count] : periods) {
                spread += seconds == 0          ? std::string{" unstated"}
                          : seconds % 3600 == 0 ? " " + std::to_string(seconds / 3600) + "h"
                                                : " " + std::to_string(seconds / 60) + "m";
                spread += "=" + std::to_string(count);
            }
            RLOG_INFO("blofin: {} of {} listed perps loaded, {} linear and {} inverse; funding "
                      "periods{}",
                      loaded.size(),
                      rows->size(),
                      loaded.size() - inverse,
                      inverse,
                      spread);
        }
        return loaded;
    }

private:
    [[nodiscard]] static bool has_logger() noexcept { return ::raikiri::log::logger() != nullptr; }

    [[nodiscard]] std::expected<std::string, exec::VenueError> get(std::string_view target) {
        const net::RestRequest request{.method = net::HttpMethod::Get, .target = target, .body = {}};
        auto response = net::await_response(state_->io, *state_->clock, rest_, request, 2ULL * k_timeout_ms + 1'000);
        if (!response.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "blofin: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (response->status == 429 || response->status == 403) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::RateLimited, response->status, "blofin: rate limited")};
        }
        const auto envelope = parse_rest_envelope(response->body);
        if (!envelope.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, response->status, to_string(envelope.error()))};
        }
        if (envelope->code != k_ok) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, envelope->code, envelope->msg)};
        }
        return std::move(response->body);
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<BlofinInstruments>(std::move(state));
}

} // namespace raikiri::venues::blofin::internal
