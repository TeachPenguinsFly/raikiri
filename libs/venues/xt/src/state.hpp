#pragma once

#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/xt/client_id.hpp>
#include <raikiri/venues/xt/endpoints.hpp>
#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/venues/xt/map.hpp>
#include <raikiri/venues/xt/rest.hpp>
#include <raikiri/venues/xt/ws.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::xt::internal {

constexpr std::size_t k_max_live_orders = 8'192;

// trailing params are not kept; the pool holds the keys frames are rebuilt from
struct OwnedSubscription {
    std::string channel;
    std::string symbol;
};

// single threaded: only the gateway loop touches it
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref) : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    std::uint64_t order_answer_timeout_ns{0};

    // spot and perp share symbols and differ in host, path and depth params; this picks one
    MarketType market{MarketType::Perp};

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;

    std::vector<OwnedSubscription> desired;

    // xt closes a socket that sends nothing for 30 s; only client messages count
    std::uint64_t next_app_ping_ns{0};

    std::vector<std::string> funding_symbols;
    std::size_t funding_cursor{0};
    std::uint64_t next_funding_poll_ns{0};
    bool funding_in_flight{false};

    // rate and mark come from different sources and the record is written whole
    std::unordered_map<std::uint32_t, FundingMemo> funding_memo;

    // null for a symbol this gateway did not list
    FundingMemo* memo_for(std::string_view symbol) {
        const auto* listed = instruments.find(symbol);
        if (listed == nullptr) {
            return nullptr;
        }
        return &funding_memo[base_of(*listed).id.symbol().raw];
    }

    net::Signer signer;

    // per tree: fapi and sapi clocks drift separately
    std::int64_t clock_offset_ns{0};
    std::int64_t spot_clock_offset_ns{0};

    std::uint64_t subscribe_refusals{0};
    std::string last_error;

    // ExecutionClient has no poll hook and xt's idle timer counts only client messages
    std::function<void(UnixNanos)> private_poll;

    // set on subscribe refusal code 2; needs a fresh key
    bool perp_key_stale{false};
    bool spot_key_stale{false};

    // keyed by venue order id; the futures order push lacks a client id and spot fills lack a side
    std::unordered_map<std::string, OrderContext> live_orders;

    bool add_subscription(std::string_view channel, std::string_view symbol) {
        for (const auto& existing : desired) {
            if (existing.channel == channel && existing.symbol == symbol) {
                return false; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{channel}, std::string{symbol});
        return true;
    }

    // takes the tree explicitly since the two clocks drift
    [[nodiscard]] UnixNanos venue_now(MarketType tree) const noexcept {
        const auto offset = tree == MarketType::Spot ? spot_clock_offset_ns : clock_offset_ns;
        const auto local = clock->wall();
        return UnixNanos{static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + offset)};
    }

    void remember(std::string_view order_id, const OrderContext& context) {
        if (order_id.empty()) {
            return;
        }
        auto key = std::string{order_id};
        if (live_orders.size() >= k_max_live_orders && !live_orders.contains(key)) {
            return; // a runaway order count must not become a runaway heap
        }
        live_orders[std::move(key)] = context;
    }

    void forget(std::string_view order_id) { live_orders.erase(std::string{order_id}); }

    [[nodiscard]] OrderContext context_for(std::string_view order_id) const {
        const auto it = live_orders.find(std::string{order_id});
        return it == live_orders.end() ? OrderContext{} : it->second;
    }
};

inline std::optional<net::RestResponse> await_response(VenueState& state,
                                                       net::RestClient& rest,
                                                       const net::RestRequest& request,
                                                       std::uint64_t timeout_ms = 10'000) {
    return net::await_response(state.io, *state.clock, rest, request, timeout_ms);
}

// each tree is synced against its own endpoint

} // namespace raikiri::venues::xt::internal
