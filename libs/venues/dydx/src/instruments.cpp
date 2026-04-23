#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/dydx/endpoints.hpp>
#include <raikiri/venues/dydx/map.hpp>
#include <raikiri/venues/dydx/wire.hpp>

#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::dydx::detail {

namespace {

constexpr std::uint32_t k_catalogue_timeout_ms = 10'000;

class DydxInstruments final : public exec::InstrumentSource {
public:
    explicit DydxInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{ep::indexer_host(state_->testnet)},
                                .port = "443",
                                .pool_size = 1,
                                .timeout_ms = k_catalogue_timeout_ms}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        if (market != MarketType::Perp) {
            return std::vector<Instrument>{};
        }
        rest_.start();
        const net::RestRequest request{
            .method = net::HttpMethod::Get, .target = ep::k_perpetual_markets, .body = {}};
        const auto response = net::await_response(
            state_->io, *state_->clock, rest_, request, 2ULL * k_catalogue_timeout_ms + 1'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (response->status == 429) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::RateLimited, response->status, "catalogue: rate limited")};
        }
        if (response->status != 200) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, response->status, "catalogue: unexpected status")};
        }

        static thread_local const auto storage = std::make_unique<MarketBuffer>();
        const auto rows = parse_perpetual_markets(response->body, *storage);
        if (!rows.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }

        const bool logging = ::raikiri::log::logger() != nullptr;
        std::vector<Instrument> loaded;
        loaded.reserve(rows->size());
        state_->instruments.reserve(rows->size());
        std::size_t inactive = 0;
        std::size_t refused = 0;
        for (const auto& row : *rows) {
            auto instrument = map_instrument(row);
            if (!instrument.has_value()) {
                if (instrument.error() == Refusal::Inactive) {
                    ++inactive;
                    continue;
                }
                ++refused;
                if (logging) {
                    RLOG_WARN("dydx: {} refused — {}", row.ticker, to_string(instrument.error()));
                }
                continue;
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                ++refused;
                continue;
            }
            loaded.push_back(*state_->instruments.find(*id));
        }
        if (logging) {
            RLOG_INFO("dydx: {} listed, {} not active, {} refused, {} loaded",
                      rows->size(),
                      inactive,
                      refused,
                      loaded.size());
        }
        return loaded;
    }

private:
    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<DydxInstruments>(std::move(state));
}

} // namespace raikiri::venues::dydx::detail
