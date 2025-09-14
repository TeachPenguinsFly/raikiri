#pragma once

// `detail`, not an anonymous namespace: a type declared in a header inside an
// anonymous namespace is a DIFFERENT type in every file that includes it.

#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bingx/client_id.hpp>
#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/venues/bingx/map.hpp>
#include <raikiri/venues/bingx/rest.hpp>
#include <raikiri/venues/bingx/ws.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::bingx::detail {

// Live orders tracked for our own id. Far above any sane working set; the cap
// exists so a venue incident cannot turn the map into the heap.
constexpr std::size_t k_max_live_orders = 8'192;

// Owned as strings: the instrument table's views outlive nothing in
// particular.
struct OwnedSubscription {
    std::string symbol;
    std::string suffix;
};

// State every bingx connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, so nothing here
// synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref)
        : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    std::uint64_t order_answer_timeout_ns{0};

    // The one market this process serves. BingX puts the two on separate
    // deployments with different frame shapes, so almost everything below
    // reads it: the host, the path, the dataType, and how a depth frame is
    // ordered.
    MarketType market{MarketType::Perp};

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    net::Signer signer;

    std::vector<OwnedSubscription> desired;

    // Symbols the gateway asked for funding on, polled round-robin.
    std::vector<std::string> funding_symbols;
    std::size_t funding_cursor{0};
    std::uint64_t next_funding_poll_ns{0};

    // Venue clock offset from /server/time. One host serves both trees, so one
    // offset is right for both — unlike xt, where two deployments drift apart.
    std::int64_t clock_offset_ns{0};

    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};

    std::uint64_t inflate_failures{0};

    // The execution client's per-tick work: ExecutionClient has no poll hook
    // of its own, and a listen key expires whether or not anyone is looking.
    std::function<void(UnixNanos)> private_poll;

    // Set when BingX says the key is dead. The socket cannot recover without a
    // fresh one, and the key rides the URL, so recovery is a reconnect.
    bool key_stale{false};

    // Our own id for a venue order id. The SPOT push carries no client order
    // id at all, so without this every spot fill would be unattributable —
    // and a restart could not tell our orders from a web order's.
    std::unordered_map<std::string, ExchangeClientOrderId> live_orders;

    void add_subscription(std::string_view symbol, std::string_view suffix) {
        for (const auto& existing : desired) {
            if (existing.symbol == symbol && existing.suffix == suffix) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{symbol}, std::string{suffix});
    }

    [[nodiscard]] UnixNanos venue_now() const noexcept {
        const auto local = clock->wall();
        return UnixNanos{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    void remember(std::string_view order_id, const ExchangeClientOrderId& coid) {
        if (order_id.empty() || coid.empty()) {
            return;
        }
        auto key = std::string{order_id};
        if (live_orders.size() >= k_max_live_orders && !live_orders.contains(key)) {
            return; // a runaway order count must not become a runaway heap
        }
        live_orders[std::move(key)] = coid;
    }

    void forget(std::string_view order_id) { live_orders.erase(std::string{order_id}); }

    [[nodiscard]] ExchangeClientOrderId remembered(std::string_view order_id) const {
        const auto it = live_orders.find(std::string{order_id});
        return it == live_orders.end() ? ExchangeClientOrderId{} : it->second;
    }
};

inline std::optional<net::RestResponse> await_response(VenueState& state,
                                                       net::RestClient& rest,
                                                       const net::RestRequest& request,
                                                       std::uint64_t timeout_ms = 10'000) {
    std::optional<net::RestResponse> result;
    if (!rest.send(request,
                   [&result](net::RestResponse&& response) { result = std::move(response); })) {
        return std::nullopt;
    }
    const auto deadline = state.clock->mono().ns + timeout_ms * 1'000'000ULL;
    while (!result.has_value() && state.clock->mono().ns < deadline) {
        if (state.io.poll() == 0) {
            state.io.run_for(5);
        }
    }
    return result;
}

// Measured once at startup: a signature stamped from a clock the venue does
// not share reads to it as a bad signature, not as a late one.
void sync_clock(VenueState& state, net::RestClient& rest, RequestBuilder& builder);

} // namespace raikiri::venues::bingx::detail
