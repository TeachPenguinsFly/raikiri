#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/lighter/catalogue.hpp>
#include <raikiri/venues/lighter/endpoints.hpp>
#include <raikiri/venues/lighter/map.hpp>

#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::lighter::internal {

namespace {

constexpr std::uint32_t k_catalogue_timeout_ms = 10'000;

class LighterInstruments final : public exec::InstrumentSource {
public:
    explicit LighterInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{ep::rest_host(state_->testnet)},
                                .port = "443",
                                .pool_size = 1,
                                .timeout_ms = k_catalogue_timeout_ms}} {
        rest_.set_admit(exec::market_data_gate(state_->rest_budget, *state_->clock));
    }

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        const net::RestRequest request{
            .method = net::HttpMethod::Get, .target = ep::k_order_book_details, .body = {}};
        // Past the transport's two timeouts in series, so the two never race.
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
        if (response->status == 429 || response->status == 405) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::RateLimited, response->status, "catalogue: rate limited")};
        }
        if (response->status != 200) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, response->status, "catalogue: unexpected status")};
        }

        static thread_local const auto storage_owner = std::make_unique<MarketBuffer>();
        const auto wire = parse_order_book_details(response->body, *storage_owner);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }

        std::vector<Instrument> loaded;
        loaded.reserve(wire->size());
        state_->instruments.reserve(wire->size());
        const std::string_view kind = market == MarketType::Spot ? "spot" : "perp";
        std::size_t inactive = 0;
        for (const auto& row : *wire) {
            auto instrument = map_instrument(row, market);
            if (!instrument.has_value()) {
                if (row.market_type == kind && row.status != "active") {
                    ++inactive;
                }
                continue;
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                continue;
            }
            state_->by_market.emplace(row.market_id, *id);
            state_->market_of.emplace(id->packed, row.market_id);
            loaded.push_back(*state_->instruments.find(*id));
        }
        if (inactive != 0 && ::raikiri::log::logger() != nullptr) {
            RLOG_INFO("lighter: {} listed market(s) are inactive and not loaded", inactive);
        }
        return loaded;
    }

private:
    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<LighterInstruments>(std::move(state));
}

} // namespace raikiri::venues::lighter::internal
