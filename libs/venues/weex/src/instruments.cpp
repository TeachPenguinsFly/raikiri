#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/weex/catalogue.hpp>
#include <raikiri/venues/weex/endpoints.hpp>
#include <raikiri/venues/weex/envelope.hpp>
#include <raikiri/venues/weex/errors.hpp>
#include <raikiri/venues/weex/map.hpp>
#include <raikiri/venues/weex/rest.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::weex::internal {

namespace {

class WeexInstruments final : public exec::InstrumentSource {
public:
    explicit WeexInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{
                    .host = std::string{state_->market == MarketType::Spot ? ep::k_spot_rest_host : rest_host()},
                    .port = "443"}} {
        // the one caller with no next tick, so a refusal fails startup
        meter_market_data(*state_, rest_);
    }

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        switch (market) {
        case MarketType::Perp:
            break;
        case MarketType::Spot:
            return fetch_spot();
        case MarketType::Future:
            return std::unexpected{exec::not_implemented("weex lists no dated futures")};
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("weex lists no options")};
        }

        rest_.start();
        // large body, so the deadline is generous
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

        load_tradable(state_->builder.api_trading_symbols(), wire->size());

        std::vector<Instrument> loaded;
        loaded.reserve(wire->size());
        state_->instruments.reserve(wire->size());
        for (const auto& entry : *wire) {
            auto instrument = map_instrument(entry, market);
            if (!instrument.has_value()) {
                continue; // not this market, or a step fixed point cannot hold
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                continue; // duplicate or table full: reported by count, not by throw
            }
            loaded.push_back(*state_->instruments.find(*id));
        }
        return loaded;
    }

private:
    // the api-tradable set; on failure it stays empty and refuses nothing
    void load_tradable(const net::RestRequest& request, std::size_t listed_total) {
        const auto listed = await_response(*state_, rest_, request, 20'000);
        if (!listed.has_value() || listed->transport_failed()) {
            RLOG_WARN("weex: no API-tradable list — an order for a symbol outside it will "
                      "be refused by the venue rather than here");
            return;
        }
        static thread_local const auto tradable_owner = std::make_unique<TradableBuffer>();
        auto& tradable = *tradable_owner;
        const auto names = parse_api_trading_symbols(listed->body, tradable);
        if (!names.has_value()) {
            RLOG_WARN("weex: API-tradable list unreadable ({})", to_string(names.error()));
            return;
        }
        state_->api_tradable.reserve(names->size());
        for (const auto& name : *names) {
            state_->api_tradable.emplace(name);
        }
        RLOG_INFO("weex: {} of {} symbols accept orders through the API", state_->api_tradable.size(), listed_total);
    }

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_spot() {
        rest_.start();
        const auto response = await_response(*state_, rest_, state_->builder.spot_instruments(), 20'000);
        if (!response.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        static thread_local const auto storage_owner = std::make_unique<SpotInstrumentBuffer>();
        const auto wire = parse_spot_exchange_info(response->body, *storage_owner);
        if (!wire.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        load_tradable(state_->builder.spot_api_trading_symbols(), wire->size());
        std::vector<Instrument> loaded;
        loaded.reserve(wire->size());
        state_->instruments.reserve(wire->size());
        for (const auto& entry : *wire) {
            const auto instrument = map_spot_instrument(entry);
            if (!instrument.has_value()) {
                continue; // halted, or a step fixed point cannot hold
            }
            if (const auto id = state_->instruments.add(*instrument); id.has_value()) {
                loaded.push_back(*state_->instruments.find(*id));
            }
        }
        return loaded;
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<WeexInstruments>(std::move(state));
}

} // namespace raikiri::venues::weex::internal
