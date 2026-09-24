#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/gate/book.hpp>
#include <raikiri/venues/gate/catalogue.hpp>
#include <raikiri/venues/gate/envelope.hpp>
#include <raikiri/venues/gate/errors.hpp>
#include <raikiri/venues/gate/map.hpp>
#include <raikiri/venues/gate/rest.hpp>
#include <raikiri/venues/gate/ws.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::gate::internal {

namespace {

// cold-start only; see GateInstruments
constexpr std::uint32_t k_catalogue_timeout_ms = 20'000;

class GateInstruments final : public exec::InstrumentSource {
public:
    explicit GateInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{rest_host(state_->testnet)},
                                .port = "443",
                                .timeout_ms = k_catalogue_timeout_ms}} {
        meter_market_data(*state_, rest_);
    }

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        net::RestRequest request{};
        switch (market) {
        case MarketType::Perp:
            request = state_->builder.contracts(k_settle_usdt);
            break;
        case MarketType::Spot:
            request = state_->builder.currency_pairs();
            break;
        case MarketType::Future:
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "gate delivery contracts are not loaded")};
        case MarketType::Option:
            // options contracts are keyed by underlying and expiry; nothing maps them
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "gate options contracts are not loaded")};
        }

        rest_.start();
        sync_clock(*state_, rest_);

        const auto response =
            await_response(*state_, rest_, request, k_catalogue_timeout_ms + 5'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected{rest_failure(response->body)};
        }
        return market == MarketType::Spot ? load_spot(response->body) : load_perp(response->body);
    }

private:
    std::expected<std::vector<Instrument>, exec::VenueError> load_perp(std::string_view body) {
        static thread_local const auto storage_owner = std::make_unique<ContractBuffer>();
        auto& storage = *storage_owner;
        const auto wire = parse_contracts(body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        return intern(
            *wire, [](const WireContract& row) { return map_instrument(row, MarketType::Perp); });
    }

    std::expected<std::vector<Instrument>, exec::VenueError> load_spot(std::string_view body) {
        static thread_local const auto storage_owner = std::make_unique<CurrencyPairBuffer>();
        auto& storage = *storage_owner;
        const auto wire = parse_currency_pairs(body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        return intern(*wire, map_currency_pair);
    }

    template <typename Row, typename Map>
    std::vector<Instrument> intern(std::span<const Row> rows, Map&& map) {
        std::vector<Instrument> loaded;
        loaded.reserve(rows.size());
        state_->instruments.reserve(state_->instruments.size() + rows.size());
        for (const auto& row : rows) {
            auto instrument = map(row);
            if (!instrument.has_value()) {
                continue; // delisting, not trading, or missing a usable step
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
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<GateInstruments>(std::move(state));
}

} // namespace raikiri::venues::gate::internal
