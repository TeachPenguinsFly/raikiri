#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/bullish/endpoints.hpp>
#include <raikiri/venues/bullish/map.hpp>
#include <raikiri/venues/bullish/wire.hpp>

#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::bullish::internal {

namespace {

constexpr std::uint32_t k_catalogue_timeout_ms = 10'000;

class BullishInstruments final : public exec::InstrumentSource {
public:
    explicit BullishInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, rest_{state_->io,
                                          net::RestConfig{.host = std::string{ep::host(state_->testnet)},
                                                          .port = "443",
                                                          .pool_size = 1,
                                                          .timeout_ms = k_catalogue_timeout_ms}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        const net::RestRequest request{.method = net::HttpMethod::Get, .target = ep::k_markets, .body = {}};
        const auto response = net::await_response(
            state_->io, *state_->clock, rest_, request, 2ULL * k_catalogue_timeout_ms + 1'000);
        if (!response.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (response->status == 429) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::RateLimited, response->status, "catalogue: rate limited")};
        }
        if (response->status != 200) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, response->status, "catalogue: unexpected status")};
        }

        static thread_local const auto storage = std::make_unique<MarketBuffer>();
        const auto rows = parse_markets(response->body, *storage);
        if (!rows.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }

        std::vector<Instrument> loaded;
        loaded.reserve(rows->size());
        state_->instruments.reserve(rows->size());
        for (const auto& row : *rows) {
            const auto instrument = map_instrument(row, market);
            if (!instrument.has_value()) {
                continue;
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                continue;
            }
            if (state_->markets.size() <= id->symbol().raw) {
                state_->markets.resize(id->symbol().raw + 1);
            }
            loaded.push_back(*state_->instruments.find(*id));
        }
        if (::raikiri::log::logger() != nullptr) {
            RLOG_INFO("bullish: {} rows listed, {} enabled {} loaded", rows->size(), loaded.size(), to_string(market));
        }
        return loaded;
    }

private:
    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<BullishInstruments>(std::move(state));
}

} // namespace raikiri::venues::bullish::internal
