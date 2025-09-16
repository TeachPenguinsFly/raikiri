#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bitget/bitget_venue.hpp>
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

namespace raikiri::venues::bitget {

using namespace detail;

// A definition of something DECLARED in `detail` must name it: `using
// namespace detail` only affects lookup, so an unqualified definition here
// quietly declares a second function in this namespace and the real one goes
// undefined at link.

void detail::sync_clock(VenueState& state, net::RestClient& rest) {
    const auto sent = state.clock->wall();
    const auto response = await_response(state, rest, state.builder.server_time(), 5'000);
    if (!response.has_value() || response->transport_failed()) {
        return; // no correction is better than a wrong one
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

// Credentials arrive as reference STRINGS, never values. A reference that
// will not resolve leaves the adapter unsigned, and every signed builder then
// refuses rather than sending an unauthenticated order.
void load_credentials(const exec::VenueConfig& config, VenueState& state) {
    // Dropped on the floor, as before: a venue factory has nowhere to report a
    // misconfigured reference to, so a typo still reads downstream as a revoked
    // key. Worth fixing, and not in the same change that moves the code.
    (void)config::Credentials{config.api_key_env, config.api_secret_env, config.passphrase_env}
        .key([&](std::string_view value) { state.signer.set_api_key(value); })
        .secret(state.signer.secret())
        .passphrase([&](std::string_view value) { (void)state.passphrase.assign(value); })
        .resolve();
}

exec::VenueAdapter
make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    const auto market = config.market;
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet, market);
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;
    load_credentials(config, *state);
    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Bitget);
    adapter.data = make_data_client(state);
    adapter.execution = make_execution_client(state);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Bitget, &make);
}

} // namespace raikiri::venues::bitget
