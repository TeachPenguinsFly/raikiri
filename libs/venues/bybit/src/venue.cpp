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

using namespace detail;

// Bybit refuses signatures outside recvWindow of ITS clock. A laptop drifting
// two seconds therefore looks exactly like bad credentials, so the offset is
// measured once at startup and applied to every signed request. Half the
// round trip is subtracted because the server's timestamp was taken somewhere
// in the middle of it.
void detail::sync_clock(VenueState& state, net::RestClient& rest) {
    net::RestRequest request{};
    request.method = net::HttpMethod::Get;
    request.target = ep::k_server_time;

    const auto before = state.clock->wall();
    // DERIVED FROM THE TRANSPORT'S OWN BOUND, never a number typed beside it: a
    // deadline equal to the client's leaves the answer to whichever timer fires
    // first, and losing that flip calls a venue silent while the pool still
    // holds its request. The extra second is the wait loop's granularity — it
    // tests its deadline only between turns of the reactor.
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
    const auto midpoint_ns =
        static_cast<std::int64_t>(before.ns) +
        (static_cast<std::int64_t>(after.ns) - static_cast<std::int64_t>(before.ns)) / 2;
    state.clock_offset_ns = server_ns - midpoint_ns;
}

exec::VenueAdapter
make_bybit(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet);
    state->budget = config.budget;
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;
    // A TIER BYBIT DOES NOT PUBLISH COSTS THE WHOLE SUBSCRIPTION, not one
    // topic: the frame comes back `error:handler not found` and every symbol
    // batched into it goes unwatched. There is nowhere here to refuse a bad
    // config to, so it is said out loud and the default stands.
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
            RLOG_WARN("bybit: book depth {} is not a tier bybit publishes — staying on {}",
                      config.book_depth,
                      k_book_depth);
        }
    }

    // Dropped on the floor, as before: a venue factory has nowhere to report a
    // misconfigured reference to, so a typo still reads downstream as a revoked
    // key. Worth fixing, and not in the same change that moves the code.
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
