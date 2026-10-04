#pragma once

// a named namespace: an anonymous one in a header is a distinct type per translation unit

#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
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
#include <unordered_set>
#include <vector>

namespace raikiri::venues::bingx::internal {

constexpr std::size_t k_max_live_orders = 8'192;

struct OwnedSubscription {
    std::string symbol;
    std::string suffix;
};

// single threaded: the gateway loop is the only caller
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref) : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    std::uint64_t order_answer_timeout_ns{0};

    // spot and perp are separate deployments; host, path, dataType and depth order follow this
    MarketType market{MarketType::Perp};

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    net::Signer signer;

    std::vector<OwnedSubscription> desired;

    // funding symbols, polled round-robin
    std::vector<std::string> funding_symbols;
    std::size_t funding_cursor{0};
    std::uint64_t next_funding_poll_ns{0};

    // one host serves both trees, so one offset
    std::int64_t clock_offset_ns{0};

    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};

    std::uint64_t inflate_failures{0};

    // per-tick exec work run from the data client's poll
    std::function<void(UnixNanos)> private_poll;

    // set when bingx reports the key dead; recovery is a reconnect
    bool key_stale{false};

    // contracts closed to api orders per apiStateOpen/apiStateClose; they stay in the catalogue
    std::unordered_set<std::string> not_orderable;

    [[nodiscard]] bool orderable(std::string_view symbol) const noexcept {
        return !not_orderable.contains(std::string{symbol});
    }

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
        return UnixNanos{static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
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
    return net::await_response(state.io, *state.clock, rest, request, timeout_ms);
}

// synced once at start; a skewed timestamp reads as a bad signature

} // namespace raikiri::venues::bingx::internal
