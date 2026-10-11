#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/edgex/endpoints.hpp>
#include <raikiri/venues/edgex/map.hpp>
#include <raikiri/venues/edgex/trade.hpp>
#include <raikiri/venues/edgex/wire.hpp>

#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::edgex::internal {

namespace {

constexpr std::uint32_t k_catalogue_timeout_ms = 10'000;

class EdgexInstruments final : public exec::InstrumentSource {
public:
    explicit EdgexInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, rest_{state_->io,
                                          net::RestConfig{.host = std::string{ep::rest_host(state_->market)},
                                                          .port = "443",
                                                          .pool_size = 1,
                                                          .timeout_ms = k_catalogue_timeout_ms}} {
        rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
    }

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        if (market != state_->market) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "edgex: one adapter serves one market")};
        }
        rest_.start();
        const net::RestRequest request{.method = net::HttpMethod::Get, .target = ep::meta_path(market), .body = {}};
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

        static thread_local const auto storage = std::make_unique<ContractBuffer>();
        const auto wire = market == MarketType::Spot ? parse_spot_catalogue(response->body, *storage)
                                                     : parse_perp_catalogue(response->body, *storage);
        if (!wire.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        // the dead v1 host answers SUCCESS with an empty catalogue
        if (wire->empty()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "catalogue: empty")};
        }
        // the order signature names the chain, the exchange contract and each contract's resolution
        if (market == MarketType::Perp && state_->signing.loaded()) {
            auto meta = parse_signing_meta(response->body);
            if (!meta.has_value()) {
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Terminal, 0, "catalogue: no signing metadata")};
            }
            state_->signing.set_meta(std::move(*meta));
        }

        std::vector<Instrument> loaded;
        loaded.reserve(wire->size());
        state_->instruments.reserve(wire->size());
        std::size_t refused = 0;
        for (const auto& row : *wire) {
            const auto instrument = map_instrument(row, market);
            if (!instrument.has_value()) {
                ++refused;
                continue;
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                ++refused;
                continue;
            }
            state_->by_contract.emplace(row.id, *id);
            state_->contract_of.emplace(id->packed, row.id);
            loaded.push_back(*state_->instruments.find(*id));
        }
        if (refused != 0 && ::raikiri::log::logger() != nullptr) {
            RLOG_INFO(
                "edgex: {} of {} listed contract(s) not loaded (disabled or off the grid)", refused, wire->size());
        }
        return loaded;
    }

private:
    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<EdgexInstruments>(std::move(state));
}

} // namespace raikiri::venues::edgex::internal
