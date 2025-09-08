#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/xt/client_id.hpp>
#include <raikiri/venues/xt/endpoints.hpp>
#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/venues/xt/map.hpp>
#include <raikiri/venues/xt/rest.hpp>
#include <raikiri/venues/xt/ws.hpp>
#include <raikiri/venues/xt/xt_venue.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::xt {

using namespace detail;

void detail::sync_clock(VenueState& state, net::RestClient& rest, RequestBuilder& builder) {
    const auto sent = state.clock->wall();
    const auto response = await_response(state, rest, builder.server_time(), 5'000);
    if (!response.has_value() || response->transport_failed()) {
        return;
    }
    const auto venue_ms = parse_server_time(response->body);
    if (!venue_ms.has_value()) {
        return;
    }
    // Half the round trip is the usual estimate of one-way delay.
    const auto received = state.clock->wall();
    const auto midpoint = sent.ns + (received.ns - sent.ns) / 2;
    state.clock_offset_ns =
        static_cast<std::int64_t>(*venue_ms * 1'000'000ULL) - static_cast<std::int64_t>(midpoint);
}

void detail::sync_spot_clock(VenueState& state, net::RestClient& rest, RequestBuilder& builder) {
    const auto sent = state.clock->wall();
    const auto response = await_response(state, rest, builder.spot_server_time(), 5'000);
    if (!response.has_value() || response->transport_failed()) {
        return;
    }
    const auto venue_ms = parse_spot_server_time(response->body);
    if (!venue_ms.has_value()) {
        return;
    }
    const auto received = state.clock->wall();
    const auto midpoint = sent.ns + (received.ns - sent.ns) / 2;
    state.spot_clock_offset_ns =
        static_cast<std::int64_t>(*venue_ms * 1'000'000ULL) - static_cast<std::int64_t>(midpoint);
}

namespace {

exec::VenueAdapter
make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock);
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;

    config::DefaultSecretProvider provider;
    if (!config.api_key_env.empty()) {
        if (const auto ref = config::SecretRef::parse(config.api_key_env); ref.has_value()) {
            config::Secret key;
            if (provider.resolve(*ref, key).has_value()) {
                state->signer.set_api_key(key.view());
            }
        }
    }
    if (!config.api_secret_env.empty()) {
        if (const auto ref = config::SecretRef::parse(config.api_secret_env); ref.has_value()) {
            (void)provider.resolve(*ref, state->signer.secret());
        }
    }

    state->market = config.market;

    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Xt);
    adapter.data = make_data_client(state);
    adapter.execution = make_execution_client(state, config.market);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Xt, &make);
}

} // namespace raikiri::venues::xt
