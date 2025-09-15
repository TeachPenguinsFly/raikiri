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
#include <raikiri/venues/gate/book.hpp>
#include <raikiri/venues/gate/envelope.hpp>
#include <raikiri/venues/gate/errors.hpp>
#include <raikiri/venues/gate/map.hpp>
#include <raikiri/venues/gate/orders.hpp>
#include <raikiri/venues/gate/rest.hpp>
#include <raikiri/venues/gate/ws.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::gate::detail {

// Live orders tracked for their leaves. Far above any sane working set; the
// cap exists so a venue incident cannot turn the map into the heap.
constexpr std::size_t k_max_live_orders = 8'192;

// A subscription the gateway asked for, owned as strings because the
// instrument table's views outlive nothing in particular.
struct OwnedSubscription {
    std::string channel;
    std::string contract;
};

// State every gate connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, which is why
// nothing here synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;
    std::uint64_t order_answer_timeout_ns{0};

    // The one market this process serves. Gate names a spot pair and its
    // perpetual identically and puts them behind different hosts, channels and
    // frame shapes, so this decides where the socket points, what it asks for,
    // and how the answers are read.
    MarketType market{MarketType::Perp};

    net::IoContext io;
    net::Signer signer;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;

    // The futures private socket demands the account's user id as the first
    // element of every subscribe payload, and answers a payload without it
    // with silence rather than an error. Spot wants no such thing.
    std::uint64_t futures_user_id{0};

    // As much of a live order as Gate states in any one frame. The orders
    // channel carries `left` and our client id; the user-trade channel carries
    // the fill and neither, so a fill cannot state its own leaves — which is
    // the number the OMS treats as authoritative. Keyed by the venue order id,
    // the one field both frames do carry.
    struct OrderMemo {
        ExchangeClientOrderId client_order_id{};
        QtyLots leaves{QtyLots::from_raw(0)};
    };

    std::unordered_map<std::string, OrderMemo> live_orders;

    // The seed rides its own pool so a book resynchronising never queues
    // behind the catalogue, which is over a megabyte.
    net::RestClient depth_rest{io, net::RestConfig{.host = std::string{rest_host(testnet)}}};
    std::unordered_map<std::uint64_t, DepthBook> books;

    std::vector<OwnedSubscription> desired;

    std::uint64_t next_app_ping_ns{0};

    // Venue clock offset, measured against /api/v4/spot/time.
    std::int64_t clock_offset_ns{0};

    // Subscriptions the venue refused.
    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};

    void add_subscription(std::string_view channel, std::string_view contract) {
        for (const auto& existing : desired) {
            if (existing.channel == channel && existing.contract == contract) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        auto at = desired.end();
        for (auto it = desired.begin(); it != desired.end(); ++it) {
            if (it->channel == channel) {
                at = it + 1;
            }
        }
        desired.emplace(at, std::string{channel}, std::string{contract});
    }

    [[nodiscard]] std::uint64_t now_s() const noexcept { return venue_now().ns / 1'000'000'000ULL; }

    // Gate refuses a signature whose Timestamp is more than 60 s from ITS
    // clock, so a laptop drifting a minute looks exactly like a bad key.
    [[nodiscard]] UnixNanos venue_now() const noexcept {
        const auto local = clock->wall();
        return UnixNanos{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    void remember(const WireOrder& wire) {
        if (wire.order_id.empty()) {
            return;
        }
        if (wire.state == WireOrderState::Cancelled || wire.state == WireOrderState::Expired ||
            wire.state == WireOrderState::Filled) {
            live_orders.erase(std::string{wire.order_id});
            return;
        }
        if (live_orders.size() >= k_max_live_orders &&
            !live_orders.contains(std::string{wire.order_id})) {
            return; // a runaway order count must not become a runaway heap
        }
        auto& memo = live_orders[std::string{wire.order_id}];
        if (const auto leaves = order_leaves(wire, instruments); leaves.has_value()) {
            memo.leaves = *leaves;
        }
        if (const auto body = client_id_from_text(wire.text); !body.empty()) {
            if (const auto id = ExchangeClientOrderId::from(body); id.has_value()) {
                memo.client_order_id = *id;
            }
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

// Gate's code is a string label and VenueError carries an integer, so the
// label travels in the message where an operator can still read it.
inline exec::VenueError rest_failure(std::string_view body) {
    const auto err = parse_rest_error(body);
    if (!err.has_value()) {
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable refusal body");
    }
    return exec::VenueError::make(classify(err->label), 0, err->label);
}

// Measured once at startup: a signature stamped from a clock the venue does
// not share reads to it as a bad signature, not as a late one.
void sync_clock(VenueState& state, net::RestClient& rest);

} // namespace raikiri::venues::gate::detail
