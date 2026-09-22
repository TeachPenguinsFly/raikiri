#pragma once

// `internal`, not an anonymous namespace: a type declared in a header inside an
// anonymous namespace is a DIFFERENT type in every file that includes it.

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

// Live orders tracked for their leaves, their side and our own id. Far above
// any sane working set; the cap exists so a venue incident cannot turn the map
// into the heap.
constexpr std::size_t k_max_live_orders = 8'192;

// A subscription the gateway asked for, owned as strings because the
// instrument table's views outlive nothing in particular. The topic's trailing
// params are not kept: this is a set that answers "already asked", and the
// pool holds the keys the frames are rebuilt from.
struct OwnedSubscription {
    std::string channel;
    std::string symbol;
};

// State every xt connection shares within one gateway process. Single threaded
// by contract: the gateway loop is the only caller, which is why nothing here
// synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref)
        : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    std::uint64_t order_answer_timeout_ns{0};

    // The one market this process serves. XT puts the two on separate
    // deployments — different host, different path, different depth params —
    // and spells their symbols identically, so this decides both where the
    // socket points and which catalogue a frame may resolve against.
    MarketType market{MarketType::Perp};

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;

    std::vector<OwnedSubscription> desired;

    // XT closes a socket that has sent nothing for 30 s however much it
    // received, and counts only CLIENT messages — the public pool's sockets
    // are no exception.
    std::uint64_t next_app_ping_ns{0};

    std::vector<std::string> funding_symbols;
    std::size_t funding_cursor{0};
    std::uint64_t next_funding_poll_ns{0};
    bool funding_in_flight{false};

    // What each contract's poll and mark channel have said so far. XT splits
    // the rate and the mark between them and the record is written whole —
    // see market_data.hpp.
    std::unordered_map<std::uint32_t, FundingMemo> funding_memo;

    // Null when the symbol is not one this gateway listed: there is nothing to
    // accumulate into and nothing the mapper could publish anyway.
    FundingMemo* memo_for(std::string_view symbol) {
        const auto* listed = instruments.find(symbol);
        if (listed == nullptr) {
            return nullptr;
        }
        return &funding_memo[base_of(*listed).id.symbol().raw];
    }

    net::Signer signer;

    // Venue clock offsets, measured separately per tree: fapi and sapi answer
    // from different deployments and one offset for both is a signature that
    // expires on whichever host drifted.
    std::int64_t clock_offset_ns{0};
    std::int64_t spot_clock_offset_ns{0};

    // Subscriptions the venue refused.
    std::uint64_t subscribe_refusals{0};
    std::string last_error;

    // The execution client's per-tick work, registered here because
    // ExecutionClient has no poll hook and XT's 30 s idle timer counts only
    // client messages — a private socket that only receives is closed.
    std::function<void(UnixNanos)> private_poll;

    // Set when a subscribe was refused with code 2: the key is dead and the
    // socket cannot recover without a fresh one.
    bool perp_key_stale{false};
    bool spot_key_stale{false};

    // As much of a live order as XT states in any one frame. The futures ORDER
    // push carries no clientOrderId and neither spot fill record carries a
    // side, so a fill cannot always state whose it is or which way it went.
    // Keyed by the venue order id, the one field every frame does carry.
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

    // Takes the tree explicitly rather than reading `market`: the two XT
    // deployments run their own clocks and drift apart, and the caller knows
    // which one the timestamp it is stamping came from.
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

// XT runs two deployments with two clocks, and they drift apart. Each is
// measured against its own endpoint; a signature stamped from the wrong one
// reads to the venue as a bad signature, not as a late one.
void sync_clock(VenueState& state, net::RestClient& rest, RequestBuilder& builder);
void sync_spot_clock(VenueState& state, net::RestClient& rest, RequestBuilder& builder);

} // namespace raikiri::venues::xt::internal
