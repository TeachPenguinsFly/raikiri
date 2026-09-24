#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bitfinex/catalogue.hpp>
#include <raikiri/venues/bitfinex/envelope.hpp>
#include <raikiri/venues/bitfinex/errors.hpp>
#include <raikiri/venues/bitfinex/map.hpp>
#include <raikiri/venues/bitfinex/rest.hpp>
#include <raikiri/venues/bitfinex/sign.hpp>
#include <raikiri/venues/bitfinex/ws.hpp>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::bitfinex::internal {

namespace {

class BitfinexInstruments final : public exec::InstrumentSource {
public:
    explicit BitfinexInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}} {
        rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
    }

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        if (const auto status = platform_status(); !status.has_value()) {
            return std::unexpected{status.error()};
        }

        const auto response = await_response(
            *state_, rest_, state_->builder.pair_info(market == MarketType::Perp), 20'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (const auto failed = as_venue_error(response->body); failed.has_value()) {
            return std::unexpected{*failed};
        }

        static thread_local const auto storage_owner = std::make_unique<PairBuffer>();

        auto& storage = *storage_owner;
        const auto wire = parse_pair_info(response->body, storage);
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
                continue; // paper pair, wrong market, or an unreadable row
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
    static std::optional<exec::VenueError> as_venue_error(std::string_view body) {
        if (!is_rest_error(body)) {
            return std::nullopt;
        }
        const auto err = parse_rest_error(body);
        if (!err.has_value()) {
            return exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(err.error()));
        }
        return exec::VenueError::make(classify(err->code), err->code, err->msg);
    }

    std::expected<void, exec::VenueError> platform_status() {
        const auto response =
            await_response(*state_, rest_, state_->builder.platform_status(), 5'000);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "platform status unavailable")};
        }
        if (const auto failed = as_venue_error(response->body); failed.has_value()) {
            return std::unexpected{*failed};
        }
        const auto status = parse_platform_status(response->body);
        if (!status.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(status.error()))};
        }
        if (*status != k_platform_operative) {
            // retryable: maintenance ends
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Retryable, *status, "platform in maintenance")};
        }
        return {};
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<BitfinexInstruments>(std::move(state));
}

} // namespace raikiri::venues::bitfinex::internal
