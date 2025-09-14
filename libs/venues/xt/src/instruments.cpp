#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/xt/catalogue.hpp>
#include <raikiri/venues/xt/client_id.hpp>
#include <raikiri/venues/xt/endpoints.hpp>
#include <raikiri/venues/xt/envelope.hpp>
#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/venues/xt/map.hpp>
#include <raikiri/venues/xt/rest.hpp>
#include <raikiri/venues/xt/ws.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::xt::detail {

namespace {

class XtInstruments final : public exec::InstrumentSource {
public:
    explicit XtInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{
                    .host = std::string{rest_host()}, .port = "443", .timeout_ms = 30'000}},
          spot_rest_{state_->io,
                     net::RestConfig{.host = std::string{spot_rest_host()},
                                     .port = "443",
                                     .timeout_ms = 30'000}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        switch (market) {
        case MarketType::Perp:
            return load_perp();
        case MarketType::Spot:
            return load_spot();
        case MarketType::Future:
            // XT lists `btc_usdt_261225` with productType "futures", but the
            // adapter serves no dated contract, so loading them would put ids
            // in the table that every order path refuses.
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "xt dated futures are not served")};
        case MarketType::Option:
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "xt lists no options")};
        }
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Terminal, 0, "xt: unknown market")};
    }

private:
    std::expected<std::vector<Instrument>, exec::VenueError> load_perp() {
        rest_.start();
        sync_clock(*state_, rest_, builder_);

        const auto response = await_response(*state_, rest_, builder_.symbol_list(), 30'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        const auto envelope = parse_rest_envelope(response->body);
        if (!envelope.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(envelope.error()))};
        }
        if (envelope->return_code != k_ok) {
            return std::unexpected{exec::VenueError::make(
                classify(envelope->code), numeric_code(envelope->code), envelope->msg)};
        }

        static thread_local InstrumentBuffer storage;
        const auto wire = parse_instruments(response->body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        return intern(
            *wire, [](const WireInstrument& row) { return map_instrument(row, MarketType::Perp); });
    }

    std::expected<std::vector<Instrument>, exec::VenueError> load_spot() {
        spot_rest_.start();
        sync_spot_clock(*state_, spot_rest_, builder_);

        const auto response =
            await_response(*state_, spot_rest_, builder_.spot_symbol_list(), 30'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        // Spot answers a refusal at HTTP 200, so `rc` is what says it failed.
        const auto envelope = parse_spot_envelope(response->body);
        if (!envelope.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(envelope.error()))};
        }
        if (envelope->rc != k_spot_ok) {
            return std::unexpected{exec::VenueError::make(
                classify_spot(envelope->mc), numeric_code(envelope->mc), envelope->mc)};
        }

        static thread_local SpotSymbolBuffer storage;
        const auto wire = parse_spot_symbols(response->body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        return intern(*wire, map_spot_instrument);
    }

    template <typename Row, typename Map>
    std::vector<Instrument> intern(std::span<const Row> rows, Map&& map) {
        std::vector<Instrument> loaded;
        loaded.reserve(rows.size());
        state_->instruments.reserve(state_->instruments.size() + rows.size());
        for (const auto& row : rows) {
            auto instrument = map(row);
            if (!instrument.has_value()) {
                continue; // not tradable through the API, or missing a step
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                // XT spells a spot pair and a perpetual IDENTICALLY, so one
                // table holds whichever market loaded first. Reported by count
                // rather than by throw, as a duplicate or a full table is.
                continue;
            }
            loaded.push_back(*state_->instruments.find(*id));
        }
        return loaded;
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
    net::RestClient spot_rest_;
    RequestBuilder builder_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<XtInstruments>(std::move(state));
}

} // namespace raikiri::venues::xt::detail
