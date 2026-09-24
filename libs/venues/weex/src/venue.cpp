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

// offset to weex's clock, less half the round trip
void internal::sync_clock(VenueState& state, net::RestClient& rest) {
    const net::RestRequest request{.method = net::HttpMethod::Get, .target = ep::k_server_time};

    const auto before = state.clock->wall();
    // derived from the transport's bound so the transport's timer fires first
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

    // three credentials or none; a bad reference reads downstream as a revoked key
    (void)config::Credentials{config.api_key_env, config.api_secret_env, config.passphrase_env}
        .key([&](std::string_view value) { state->signer.set_api_key(value); })
        .secret(state->signer.secret())
        // the passphrase travels in a header, so it may be copied out
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
