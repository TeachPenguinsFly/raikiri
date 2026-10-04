#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bitget/catalogue.hpp>
#include <raikiri/venues/bitget/envelope.hpp>
#include <raikiri/venues/bitget/errors.hpp>
#include <raikiri/venues/bitget/map.hpp>
#include <raikiri/venues/bitget/rest.hpp>
#include <raikiri/venues/bitget/ws.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::bitget::internal {

namespace {

class BitgetInstruments final : public exec::InstrumentSource {
public:
    explicit BitgetInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        switch (market) {
        case MarketType::Spot:
            return load_spot();
        case MarketType::Perp:
            return load_mix(market);
        case MarketType::Future:
            // the mapper reads perpetual rows only, so delivery would return an empty list
            return std::unexpected{exec::not_implemented("bitget delivery futures catalogue")};
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("bitget options catalogue")};
        }
        return std::unexpected{exec::not_implemented("bitget catalogue")};
    }

private:
    static constexpr std::uint64_t k_catalogue_timeout_ms = 20'000;

    template <typename Rows, typename Map> std::vector<Instrument> intern(const Rows& rows, Map&& map) {
        std::vector<Instrument> loaded;
        loaded.reserve(rows.size());
        state_->instruments.reserve(rows.size());
        for (const auto& entry : rows) {
            auto instrument = map(entry);
            if (!instrument.has_value()) {
                continue; // not tradable, not this market, or missing a step
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                continue; // duplicate or table full; reported by count
            }
            loaded.push_back(*state_->instruments.find(*id));
        }
        return loaded;
    }

    std::expected<std::vector<Instrument>, exec::VenueError> load_mix(MarketType market) {
        const auto body = fetch_checked(
            *state_, rest_, state_->builder.contracts(state_->product()), k_catalogue_timeout_ms);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<ContractBuffer>();
        auto& storage = *storage_owner;
        const auto wire = parse_contracts(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        return intern(*wire, [market](const WireContract& row) { return map_instrument(row, market); });
    }

    std::expected<std::vector<Instrument>, exec::VenueError> load_spot() {
        const auto body = fetch_checked(*state_, rest_, state_->builder.spot_symbols(), k_catalogue_timeout_ms);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        auto storage = std::make_unique<SpotSymbolBuffer>();
        const auto wire = parse_spot_symbols(*body, *storage);
        if (!wire.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        return intern(*wire, [](const WireSpotSymbol& row) { return map_instrument(row); });
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<BitgetInstruments>(std::move(state));
}

} // namespace raikiri::venues::bitget::internal
