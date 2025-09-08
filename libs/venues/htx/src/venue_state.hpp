#pragma once

// What the three htx clients share, and nothing else.
//
// The public socket, the private socket and the catalogue are separate
// concerns that happen to run against one connection state. Only that state
// crosses the boundary; each client keeps its own constants and its own
// helpers in its own translation unit, where a reader looking for one of them
// is not made to scroll past the other two.
//
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
#include <raikiri/venues/htx/errors.hpp>
#include <raikiri/venues/htx/map.hpp>
#include <raikiri/venues/htx/rest.hpp>
#include <raikiri/venues/htx/ws.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::htx::detail {

// Live orders tracked for their leaves and their identity. Far above any sane
// working set; the cap exists so a venue incident cannot turn the map into the
// heap.
constexpr std::size_t k_max_live_orders = 8'192;

// A subscription the gateway asked for, owned as strings because the
// instrument table's views outlive nothing in particular.
struct OwnedSubscription {
    std::string code;
    std::string suffix;
};

// State every htx connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, which is why
// nothing here synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref)
        : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    std::uint64_t order_answer_timeout_ns{0};

    net::IoContext io;
    net::Signer signer;
    exec::InstrumentTable instruments;

    Protocol protocol;

    std::vector<OwnedSubscription> desired;
    std::vector<std::string> funding_codes;

    // Venue clock offset from /api/v1/timestamp.
    std::int64_t clock_offset_ns{0};

    std::uint64_t subscribe_refusals{0};
    std::string last_error_code;

    std::uint64_t inflate_failures{0};

    // As much of a live order as HTX states in any one frame, keyed by the
    // venue order id — the only field every private frame on both surfaces
    // carries. Spot's REST match rows name no client order id at all, and a
    // fill that cannot say whose it is routes nowhere.
    struct OrderMemo {
        ClientOrderId client_order_id{};
        ExchangeClientOrderId exchange_client_order_id{};
        QtyLots leaves{QtyLots::from_raw(0)};
    };

    std::unordered_map<std::string, OrderMemo> live_orders;

    // What a perp order was actually sent as. The swap's client_order_id is an
    // int64, so the ULID the OMS asks query_order about was never on the wire;
    // this is the only place the two are tied together, and the contract code
    // beside it is the other half the single-order read demands.
    struct SwapTarget {
        std::int64_t client_order_id{0};
        std::string contract_code;
    };

    std::unordered_map<std::string, SwapTarget> swap_targets;

    void add_subscription(std::string_view code, std::string_view suffix) {
        for (const auto& existing : desired) {
            if (existing.code == code && existing.suffix == suffix) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{code}, std::string{suffix});
    }

    // HTX refuses a signature whose Timestamp is more than a few minutes from
    // ITS clock, so a laptop drifting looks exactly like a bad key.
    [[nodiscard]] UnixNanos venue_now() const noexcept {
        const auto local = clock->wall();
        return UnixNanos{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    void remember(const WireOrder& wire) {
        if (wire.order_id.empty()) {
            return;
        }
        std::string key{wire.order_id};
        if (wire.state == WireOrderState::Canceled || wire.state == WireOrderState::Filled ||
            wire.state == WireOrderState::Rejected) {
            live_orders.erase(key);
            return;
        }
        if (live_orders.size() >= k_max_live_orders && !live_orders.contains(key)) {
            return; // a runaway order count must not become a runaway heap
        }
        const auto* instrument = instruments.find(wire.contract_code);
        if (instrument == nullptr) {
            return;
        }
        auto& memo = live_orders[key];
        if (const auto leaves = order_leaves(wire, instruments); leaves.has_value()) {
            memo.leaves = *leaves;
        }
        const auto identity =
            map_identity(wire.client_order_id, wire.order_id, base_of(*instrument).id.market());
        if (!identity.has_value()) {
            return;
        }
        if (identity->client_order_id.valid()) {
            memo.client_order_id = identity->client_order_id;
        }
        if (!identity->exchange_client_order_id.empty()) {
            memo.exchange_client_order_id = identity->exchange_client_order_id;
        }
    }

    [[nodiscard]] OrderMemo memo_for(std::string_view order_id) const {
        const auto it = live_orders.find(std::string{order_id});
        return it == live_orders.end() ? OrderMemo{} : it->second;
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

// Every market this adapter can act on, named exhaustively so Option is a case
// the compiler forces a decision about rather than something a default arm
// swallows. An empty answer here would read as "the venue holds nothing".
inline std::optional<exec::VenueError> refuse_market(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
    case MarketType::Perp:
        return std::nullopt;
    case MarketType::Future:
        return exec::not_implemented("htx dated futures order entry");
    case MarketType::Option:
        // HTX lists options on their own host and API, keyed by an underlying
        // and an expiry. Nothing maps an OptionInstrument onto that yet.
        return exec::not_implemented("htx options order entry");
    }
    return exec::not_implemented("htx: unknown market");
}

// Measured once at startup: a signature stamped from a clock the venue does
// not share reads to it as a bad signature, not as a late one.
void sync_clock(VenueState& state, net::RestClient& rest);

} // namespace raikiri::venues::htx::detail
