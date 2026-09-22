#include <raikiri/exec/instrument_source.hpp>
#include <raikiri/venues/backpack/catalogue.hpp>
#include <raikiri/venues/backpack/endpoints.hpp>
#include <raikiri/venues/backpack/envelope.hpp>
#include <raikiri/venues/backpack/map.hpp>

#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::backpack::internal {

namespace {

class BackpackInstruments final : public exec::InstrumentSource {
public:
    explicit BackpackInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{ep::rest_host(state_->testnet)}}} {
        meter_market_data(*state_, rest_);
    }

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        const auto response = await_response(*state_, rest_, state_->builder.markets(), 20'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (response->status != 200) {
            const auto err = parse_rest_error(response->body);
            const auto cls = response->status == 429 ? exec::ErrorClass::RateLimited
                                                     : exec::ErrorClass::Terminal;
            return std::unexpected{exec::VenueError::make(
                cls,
                response->status,
                err.has_value() && !err->message.empty() ? err->message : "catalogue refused")};
        }

        static thread_local const auto storage_owner = std::make_unique<MarketBuffer>();
        const auto wire = parse_markets(response->body, *storage_owner);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }

        std::vector<Instrument> loaded;
        loaded.reserve(wire->size());
        for (const auto& row : *wire) {
            const auto instrument = map_instrument(row, market);
            if (!instrument.has_value()) {
                continue;
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                continue;
            }
            loaded.push_back(*state_->instruments.find(*id));
        }
        return loaded;
    }

private:
    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<BackpackInstruments>(std::move(state));
}

} // namespace raikiri::venues::backpack::internal
