#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/weex/endpoints.hpp>
#include <raikiri/venues/weex/envelope.hpp>
#include <raikiri/venues/weex/weex_venue.hpp>

#include <cstdint>
#include <memory>
#include <string>

#include "clients.hpp"

namespace raikiri::venues::weex {

using internal::VenueState;
using internal::await_response;
using internal::make_data_client;
using internal::make_execution_client;
using internal::make_instrument_source;

// WEEX refuses a signature stamped more than 30 SECONDS from its own clock, so
// a laptop drifting a minute looks exactly like a revoked key. The offset is
// measured once at start and applied to every signed request. Half the round
// trip is subtracted because the server's timestamp was taken somewhere in the
// middle of it.
void internal::sync_clock(VenueState& state, net::RestClient& rest) {
    const net::RestRequest request{.method = net::HttpMethod::Get, .target = ep::k_server_time};

    const auto before = state.clock->wall();
    // DERIVED FROM THE TRANSPORT'S OWN BOUND, never a number typed beside it: a
    // deadline equal to the client's leaves the answer to whichever timer fires
    // first, and losing that flip calls a venue silent while the pool still
    // holds its request.
    const auto response = await_response(state, rest, request, rest.max_wait_ms() + 1'000);
    if (!response.has_value() || response->transport_failed()) {
        return; // no correction is better than a wrong one
    }
    const auto after = state.clock->wall();
    const auto server_ms = parse_server_time(response->body);
    if (!server_ms.has_value() || *server_ms == 0) {
        return;
    }
    const auto server_ns = static_cast<std::int64_t>(*server_ms) * 1'000'000;
    const auto midpoint_ns =
        static_cast<std::int64_t>(before.ns) +
        (static_cast<std::int64_t>(after.ns) - static_cast<std::int64_t>(before.ns)) / 2;
    state.clock_offset_ns = server_ns - midpoint_ns;
}

namespace {

exec::VenueAdapter
make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock);
    state->budget = config.budget;
    state->market = config.market;
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;

    // THREE CREDENTIALS OR NONE. WEEX signs with a passphrase beside the key
    // and the secret, and make_venue refuses a configuration that names two of
    // the three — the caps row's `needs_passphrase` is what tells it to. A
    // gateway that names none is asking for market data and is refused nothing.
    //
    // A misconfigured reference is dropped on the floor, as it is on every
    // other venue here: a factory has nowhere to report one to, so a typo still
    // reads downstream as a revoked key. Worth fixing, and not here.
    (void)config::Credentials{config.api_key_env, config.api_secret_env, config.passphrase_env}
        .key([&](std::string_view value) { state->signer.set_api_key(value); })
        .secret(state->signer.secret())
        // A PUBLIC VALUE BY CONSTRUCTION: it travels in a header on every
        // request, which is why it may be copied out while the secret never is.
        .passphrase([&](std::string_view value) { state->passphrase.assign(value); })
        .resolve();

    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Weex);
    adapter.data = make_data_client(state);
    adapter.execution = make_execution_client(state);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Weex, &make);
}

} // namespace raikiri::venues::weex
