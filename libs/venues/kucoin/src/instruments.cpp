#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/kucoin/catalogue.hpp>
#include <raikiri/venues/kucoin/envelope.hpp>
#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/venues/kucoin/map.hpp>
#include <raikiri/venues/kucoin/rest.hpp>
#include <raikiri/venues/kucoin/ws.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::kucoin::internal {

namespace {

void sync_clock(VenueState& state, net::RestClient& rest) {
    const auto sent = state.clock->wall();
    const auto response = await_response(state, rest, state.builder.server_time(), 5'000);
    if (!response.has_value() || response->transport_failed()) {
        return;
    }
    const auto venue_ms = parse_server_time(response->body);
    if (!venue_ms.has_value()) {
        return;
    }
    const auto received = state.clock->wall();
    const auto midpoint = sent.ns + (received.ns - sent.ns) / 2;
    state.clock_offset_ns =
        static_cast<std::int64_t>(*venue_ms * 1'000'000ULL) - static_cast<std::int64_t>(midpoint);
}

class KucoinInstruments final : public exec::InstrumentSource {
public:
    explicit KucoinInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}},
          spot_rest_{
              state_->io,
              net::RestConfig{.host = std::string{rest_host(MarketType::Spot)}, .port = "443"}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        switch (market) {
        case MarketType::Spot:
            return fetch_spot();
        case MarketType::Perp:
            return fetch_futures();
        case MarketType::Future:
            // dated contracts on the futures host are all inverse
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "kucoin dated futures are not modelled")};
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("kucoin options order entry")};
        }
        return std::unexpected{exec::not_implemented("kucoin options order entry")};
    }

private:
    std::expected<std::vector<Instrument>, exec::VenueError> fetch_futures() {
        rest_.start();
        // kucoin refuses signatures more than 5 s off its clock
        sync_clock(*state_, rest_);

        const auto body =
            usable_body(await_response(*state_, rest_, state_->builder.contracts_active(), 20'000));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local const auto storage_owner = std::make_unique<ContractBuffer>();

        auto& storage = *storage_owner;
        const auto wire = parse_contracts(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }

        std::vector<Instrument> loaded;
        loaded.reserve(wire->size());
        state_->instruments.reserve(wire->size());
        for (const auto& entry : *wire) {
            auto instrument = map_instrument(entry, MarketType::Perp);
            if (!instrument.has_value()) {
                if (entry.status == "Open" && entry.type == "FFWCSX" && !entry.is_inverse) {
                    RLOG_WARN("kucoin: {} dropped — tick {} or lot {} x {} is finer than 1e-9 "
                              "or unreadable",
                              entry.symbol, entry.tick_size, entry.lot_size, entry.multiplier);
                }
                continue;
            }
            admit(*instrument, loaded);
        }
        return loaded;
    }

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_spot() {
        spot_rest_.start();
        const auto body = usable_body(
            await_response(*state_, spot_rest_, state_->builder.spot_symbols(), 20'000));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        auto storage = std::make_unique<SpotSymbolBuffer>();
        const auto wire = parse_spot_symbols(*body, *storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }

        std::vector<Instrument> loaded;
        loaded.reserve(wire->size());
        state_->instruments.reserve(wire->size());
        for (const auto& entry : *wire) {
            auto instrument = map_spot_symbol(entry);
            if (!instrument.has_value()) {
                continue; // suspended, or missing a usable increment
            }
            admit(*instrument, loaded);
        }
        return loaded;
    }

    void admit(const Instrument& instrument, std::vector<Instrument>& loaded) {
        const auto id = state_->instruments.add(instrument);
        if (!id.has_value()) {
            return; // duplicate or table full: reported by count, not by throw
        }
        loaded.push_back(*state_->instruments.find(*id));
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
    net::RestClient spot_rest_;
};

} // namespace

std::unique_ptr<exec::InstrumentSource> make_instrument_source(std::shared_ptr<VenueState> state) {
    return std::make_unique<KucoinInstruments>(std::move(state));
}

} // namespace raikiri::venues::kucoin::internal
