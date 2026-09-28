#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/bitunix/endpoints.hpp>
#include <raikiri/venues/bitunix/map.hpp>
#include <raikiri/venues/bitunix/wire.hpp>

#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::bitunix::internal {

namespace {

constexpr std::uint32_t k_catalogue_timeout_ms = 10'000;

class BitunixInstruments final : public exec::InstrumentSource {
public:
    explicit BitunixInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, rest_{state_->io,
                                          net::RestConfig{.host = std::string{state_->rest_host()},
                                                          .port = "443",
                                                          .pool_size = 1,
                                                          .timeout_ms = k_catalogue_timeout_ms}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        if (market != MarketType::Perp && market != MarketType::Spot) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "bitunix lists perpetuals and spot only")};
        }
        rest_.start();
        const net::RestRequest request{.method = net::HttpMethod::Get,
                                       .target = market == MarketType::Spot ? ep::k_spot_pairs : ep::k_trading_pairs,
                                       .body = {}};
        const auto response = net::await_response(
            state_->io, *state_->clock, rest_, request, 2ULL * k_catalogue_timeout_ms + 1'000);
        if (!response.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        const auto status = parse_status(response->body);
        if (status.has_value() && status->code == k_rate_limited) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::RateLimited, status->code, "catalogue: rate limited")};
        }
        if (response->status != 200 || !status.has_value() || status->code != k_ok) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal,
                                                          status.has_value() ? status->code : response->status,
                                                          "catalogue: refused")};
        }

        static thread_local const auto storage = std::make_unique<PairBuffer>();
        const auto rows = market == MarketType::Spot ? parse_spot_pairs(response->body, *storage)
                                                     : parse_pairs(response->body, *storage);
        if (!rows.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }

        std::vector<Instrument> loaded;
        loaded.reserve(rows->size());
        state_->instruments.reserve(rows->size());
        for (const auto& row : *rows) {
            const auto instrument = map_pair(row, market);
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
            RLOG_INFO("bitunix: {} rows listed, {} {} loaded as api-tradable",
                      rows->size(),
                      loaded.size(),
                      to_string(market));
        }
        return loaded;
    }

private:
    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<BitunixInstruments>(std::move(state));
}

} // namespace raikiri::venues::bitunix::internal
