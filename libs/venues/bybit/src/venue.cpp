#include <raikiri/config/credentials.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bybit/bybit_venue.hpp>
#include <raikiri/venues/bybit/endpoints.hpp>
#include <raikiri/venues/bybit/envelope.hpp>
#include <raikiri/venues/bybit/errors.hpp>
#include <raikiri/venues/bybit/map.hpp>
#include <raikiri/venues/bybit/rest.hpp>
#include <raikiri/venues/bybit/ws.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::bybit {

using internal::await_response;
using internal::make_data_client;
using internal::make_execution_client;
using internal::make_instrument_source;
using internal::VenueState;

// bybit checks recvWindow against its own clock; the offset is measured at startup, less half the round trip
void internal::sync_clock(VenueState& state, net::RestClient& rest) {
    net::RestRequest request{};
    request.method = net::HttpMethod::Get;
    request.target = ep::k_server_time;

    const auto before = state.clock->wall();
    // the transport's bound plus the wait loop's one-second granularity
    const auto response = await_response(state, rest, request, rest.max_wait_ms() + 1'000);
    if (!response.has_value() || response->transport_failed()) {
        return; // no correction is better than a wrong one
    }
    const auto after = state.clock->wall();
    const auto envelope = parse_envelope(response->body);
    if (!envelope.has_value() || envelope->ret_code != k_ret_ok || envelope->time_ms == 0) {
        return;
    }
    const auto server_ns = static_cast<std::int64_t>(envelope->time_ms) * 1'000'000;
    const auto midpoint_ns = static_cast<std::int64_t>(before.ns) +
                             (static_cast<std::int64_t>(after.ns) - static_cast<std::int64_t>(before.ns)) / 2;
    state.clock_offset_ns = server_ns - midpoint_ns;
}

exec::VenueAdapter make_bybit(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet);
    state->budget = config.budget;
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;
    // an unpublished tier fails the whole subscribe frame; warn and keep the default
    if (config.book_depth != 0) {
        if (is_book_tier(config.book_depth)) {
            state->book_depth = config.book_depth;
            RLOG_INFO("bybit: book tier {} — {} ms between pushes",
                      config.book_depth,
                      config.book_depth == 1     ? 10
                      : config.book_depth == 50  ? 20
                      : config.book_depth == 200 ? 100
                                                 : 200);
        } else {
            RLOG_WARN(
                "bybit: book depth {} is not a tier bybit publishes — staying on {}", config.book_depth, k_book_depth);
        }
    }

    // credential errors are dropped here; a typo reads downstream as a revoked key
    (void)config::Credentials{config.api_key_env, config.api_secret_env}
        .key([&](std::string_view value) { state->signer.set_api_key(value); })
        .secret(state->signer.secret())
        .resolve();

    const auto market = config.market;
    exec::VenueAdapter adapter;
    adapter.caps = exec::caps_of(VenueId::Bybit);
    adapter.data = make_data_client(state, market);
    adapter.execution = make_execution_client(state);
    adapter.instruments = make_instrument_source(state);
    return adapter;
}

void register_adapter() {
    exec::register_venue(VenueId::Bybit, &make_bybit);
}

} // namespace raikiri::venues::bybit
