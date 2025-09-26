#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/weex/catalogue.hpp>
#include <raikiri/venues/weex/envelope.hpp>
#include <raikiri/venues/weex/errors.hpp>
#include <raikiri/venues/weex/map.hpp>
#include <raikiri/venues/weex/rest.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::weex::detail {

namespace {

class WeexInstruments final : public exec::InstrumentSource {
public:
    explicit WeexInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}} {
        // One call, weight 1, against a bucket still at capacity — and it is
        // the one caller with no next tick, so a refusal here surfaces as a
        // gateway that will not start rather than a retry.
        meter_market_data(*state_, rest_);
    }

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        // ANSWERED BEFORE THE CALL. WEEX's contract tree lists perpetuals and
        // nothing else, and an empty catalogue reads as "this venue lists
        // nothing" — a very different claim from "this adapter does not load
        // that market".
        switch (market) {
        case MarketType::Perp:
            break;
        case MarketType::Spot:
            return std::unexpected{exec::not_implemented("weex spot is a different exchange")};
        case MarketType::Future:
            return std::unexpected{exec::not_implemented("weex lists no dated futures")};
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("weex lists no options")};
        }

        rest_.start();
        // 685 KiB on 2026-09-07, so the deadline is the body's rather than the
        // round trip's.
        const auto response = await_response(*state_, rest_, state_->builder.instruments(), 20'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (const auto err = parse_rest_error(response->body);
            err.has_value() && err->code != k_ok) {
            return std::unexpected{
                exec::VenueError::make(classify(err->code), err->code, err->msg)};
        }

        static thread_local InstrumentBuffer storage;
        const auto wire = parse_exchange_info(response->body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }

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
    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<WeexInstruments>(std::move(state));
}

} // namespace raikiri::venues::weex::detail
