#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/htx/envelope.hpp>
#include <raikiri/venues/htx/errors.hpp>
#include <raikiri/venues/htx/map.hpp>
#include <raikiri/venues/htx/rest.hpp>
#include <raikiri/venues/htx/ws.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::htx::detail {

namespace {

class HtxInstruments final : public exec::InstrumentSource {
public:
    explicit HtxInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{rest_host()},
                                .port = "443",
                                // Measured 13.3 s on a cold endpoint against a
                                // 5 s default, so the gateway refused to start
                                // with a timeout that reads as the venue being
                                // down. It warms to 0.2 s afterwards.
                                .timeout_ms = 20'000}},
          spot_rest_{state_->io,
                     net::RestConfig{.host = std::string{spot_rest_host()},
                                     .port = "443",
                                     .timeout_ms = 20'000}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const bool spot = market == MarketType::Spot;
        auto& rest = spot ? spot_rest_ : rest_;
        rest_.start();
        spot_rest_.start();
        sync_clock(*state_, rest_);

        const auto request = spot ? RequestBuilder::spot_symbols() : RequestBuilder::contracts();
        const auto response = await_response(*state_, rest, request, 20'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (response->status != 200) {
            return std::unexpected{exec::VenueError::make(
                classify_http(response->status), response->status, "catalogue: http error")};
        }
        const auto envelope = parse_rest_envelope(response->body);
        if (!envelope.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(envelope.error()))};
        }
        if (!envelope->ok) {
            return std::unexpected{
                exec::VenueError::make(classify(envelope->code), 0, envelope->msg)};
        }
        return spot ? load_spot(response->body) : load_perp(response->body, market);
    }

private:
    std::expected<std::vector<Instrument>, exec::VenueError> load_perp(std::string_view body,
                                                                       MarketType market) {
        static thread_local ContractBuffer storage;
        const auto wire = parse_contracts(body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        return intern(*wire,
                      [market](const WireContract& row) { return map_instrument(row, market); });
    }

    std::expected<std::vector<Instrument>, exec::VenueError> load_spot(std::string_view body) {
        static thread_local SpotSymbolBuffer storage;
        const auto wire = parse_spot_symbols(body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        return intern(*wire, map_spot_symbol);
    }

    template <typename Row, typename Map>
    std::vector<Instrument> intern(std::span<const Row> rows, Map&& map) {
        std::vector<Instrument> loaded;
        loaded.reserve(rows.size());
        state_->instruments.reserve(state_->instruments.size() + rows.size());
        for (const auto& row : rows) {
            auto instrument = map(row);
            if (!instrument.has_value()) {
                continue; // not listed, not this market, or an unusable step
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                continue; // duplicate or table full: reported by count, not by throw
            }
            loaded.push_back(*state_->instruments.find(*id));
        }
        return loaded;
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
    net::RestClient spot_rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<HtxInstruments>(std::move(state));
}

} // namespace raikiri::venues::htx::detail
