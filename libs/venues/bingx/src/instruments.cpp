#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bingx/client_id.hpp>
#include <raikiri/venues/bingx/envelope.hpp>
#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/venues/bingx/map.hpp>
#include <raikiri/venues/bingx/rest.hpp>
#include <raikiri/venues/bingx/ws.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::bingx::detail {

namespace {

class BingxInstruments final : public exec::InstrumentSource {
public:
    explicit BingxInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        sync_clock(*state_, rest_, builder_);

        switch (market) {
        case MarketType::Perp:
            return load_perp();
        case MarketType::Spot:
            return load_spot();
        case MarketType::Future:
        case MarketType::Option:
            // Neither tree lists them, so an empty vector would be a claim
            // about a market BingX does not have.
            return std::unexpected{exec::not_implemented(to_string(*market_refusal(market)))};
        }
        return std::unexpected{exec::not_implemented("bingx: unknown market")};
    }

private:
    [[nodiscard]] std::expected<std::string, exec::VenueError> read(const net::RestRequest& request,
                                                                    std::uint64_t timeout_ms) {
        const auto response = await_response(*state_, rest_, request, timeout_ms);
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
        if (envelope->code != k_ok) {
            return std::unexpected{
                exec::VenueError::make(classify(envelope->code), envelope->code, envelope->msg)};
        }
        return response->body;
    }

    std::expected<std::vector<Instrument>, exec::VenueError> load_perp() {
        const auto body = read(builder_.contracts(), 20'000);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local ContractBuffer storage;
        const auto wire = parse_contracts(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        return intern(
            *wire, [](const WireContract& row) { return map_instrument(row, MarketType::Perp); });
    }

    std::expected<std::vector<Instrument>, exec::VenueError> load_spot() {
        const auto body = read(builder_.spot_symbols(), 30'000);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local SpotSymbolBuffer storage;
        const auto wire = parse_spot_symbols(*body, storage);
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
                continue; // not online, closed to the API, or missing a step
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                // BingX spells a spot pair and a perpetual IDENTICALLY, so one
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
    RequestBuilder builder_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<BingxInstruments>(std::move(state));
}

} // namespace raikiri::venues::bingx::detail
