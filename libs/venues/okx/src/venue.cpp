#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/okx/envelope.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/venues/okx/map.hpp>
#include <raikiri/venues/okx/okx_venue.hpp>
#include <raikiri/venues/okx/rest.hpp>
#include <raikiri/venues/okx/ws.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::okx {

using internal::VenueState;
using internal::await_response;
using internal::make_data_client;
using internal::make_execution_client;
using internal::make_instrument_source;

void internal::sync_clock(VenueState& state, net::RestClient& rest) {
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

namespace {

// credentials are env var names; an unresolved one leaves the adapter market-data only
void load_credentials(VenueState& state, const exec::VenueConfig& config) {
    // okx needs the passphrase; without it signed requests get 50104
    (void)config::Credentials{config.api_key_env, config.api_secret_env, config.passphrase_env}
        .key([&](std::string_view value) { state.signer.set_api_key(value); })
        .secret(state.signer.secret())
        .passphrase([&](std::string_view value) { (void)state.passphrase.assign(value); })
        .resolve();
}

exec::VenueAdapter
make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet);
    state->budget = config.budget;
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;
    state->spot_enabled = config.market == MarketType::Spot;
    state->perp_enabled = config.market == MarketType::Perp;
    load_credentials(*state, config);

    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Okx);
    adapter.data = make_data_client(state);
    adapter.execution = make_execution_client(state);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Okx, &make);
}

} // namespace raikiri::venues::okx
