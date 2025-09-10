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
#include <raikiri/venues/mexc/errors.hpp>
#include <raikiri/venues/mexc/map.hpp>
#include <raikiri/venues/mexc/rest.hpp>
#include <raikiri/venues/mexc/spot_wire.hpp>
#include <raikiri/venues/mexc/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::mexc::detail {

// Live orders tracked for their leaves. Far above any sane working set; the
// cap exists so a venue incident cannot turn the map into the heap.
constexpr std::size_t k_max_live_orders = 8'192;

// A subscription the gateway asked for, owned as strings because the
// instrument table's views outlive nothing in particular.
struct OwnedSubscription {
    std::string channel;
    std::string symbol;
    std::uint32_t limit{0};
};

// State every mexc connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, which is why
// nothing here synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref)
        : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    std::uint64_t order_answer_timeout_ns{0};

    // The one market this process serves. MEXC's two trees share nothing: not
    // the host, not the subscription shape, not even the encoding — futures is
    // JSON and spot is protobuf.
    MarketType market{MarketType::Perp};

    net::IoContext io;
    net::Signer signer;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;

    std::vector<OwnedSubscription> desired;

    std::uint64_t next_app_ping_ns{0};

    // The execution client's socket, registered so the data client's poll can
    // keep it alive too. ExecutionClient has no poll hook and MEXC's 60 s idle
    // timer counts only CLIENT messages — a socket that only receives is
    // closed — so the one timer this adapter has drives both connections.
    net::WsClient* private_client{nullptr};

    // Venue clock offsets, measured separately per tree: /api/v1 and /api/v3
    // check a request against their own clock, and one offset for both is a
    // signature that expires on whichever host drifted.
    std::int64_t clock_offset_ns{0};
    std::int64_t spot_clock_offset_ns{0};

    // Subscriptions the venue refused.
    std::uint64_t subscribe_refusals{0};

    // As much of a live futures order as MEXC states in any one frame. The
    // order push carries `vol`/`dealVol` and our externalOid; the deal push
    // carries the fill and neither, so a fill cannot state its own leaves —
    // which is the number the OMS treats as authoritative. Keyed by the venue
    // order id, the one field both frames do carry.
    struct OrderMemo {
        ExchangeClientOrderId client_order_id{};
        QtyLots leaves{QtyLots::from_raw(0)};
    };

    std::unordered_map<std::string, OrderMemo> live_orders;

    void add_subscription(std::string_view channel, std::string_view symbol, std::uint32_t limit) {
        for (const auto& existing : desired) {
            if (existing.channel == channel && existing.symbol == symbol) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{channel}, std::string{symbol}, limit);
    }

    // Takes the tree explicitly rather than reading `market`: the caller knows
    // which deployment stamped the timestamp it is adjusting.
    [[nodiscard]] UnixNanos venue_now(MarketType tree) const noexcept {
        const auto offset = tree == MarketType::Spot ? spot_clock_offset_ns : clock_offset_ns;
        const auto local = clock->wall();
        return UnixNanos{static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + offset)};
    }

    void remember(const WireFuturesOrder& wire) {
        if (wire.order_id.empty()) {
            return;
        }
        // 3 filled, 4 cancelled, 5 invalid: nothing further can reference it.
        if (wire.state >= 3) {
            live_orders.erase(std::string{wire.order_id});
            return;
        }
        const auto* instrument = instruments.find(wire.symbol);
        if (instrument == nullptr) {
            return;
        }
        auto key = std::string{wire.order_id};
        if (live_orders.size() >= k_max_live_orders && !live_orders.contains(key)) {
            return; // a runaway order count must not become a runaway heap
        }
        auto& memo = live_orders[key];
        if (const auto report = map_futures_open_order(wire, instruments); report.has_value()) {
            memo.leaves = report->leaves;
            if (!report->exchange_client_order_id.empty()) {
                memo.client_order_id = report->exchange_client_order_id;
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

// The two trees report a refusal differently: futures wraps it in
// {"success":false,"code":..,"message":..} and spot answers a bare
// {"code":..,"msg":..}. Reading one with the other's parser loses the code,
// and the code is what decides retry from halt.
inline exec::VenueError rest_failure(std::string_view body, MarketType market) {
    if (market == MarketType::Spot) {
        const auto err = parse_spot_error(body);
        if (!err.has_value()) {
            return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable refusal body");
        }
        return exec::VenueError::make(classify_spot(err->code), err->code, err->msg);
    }
    const auto envelope = parse_rest_envelope(body);
    if (!envelope.has_value()) {
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable refusal body");
    }
    return exec::VenueError::make(classify(envelope->code), envelope->code, envelope->msg);
}

// Measured once at startup: a signature stamped from a clock the venue does
// not share reads to it as a bad signature, not as a late one.
void sync_clock(VenueState& state, net::RestClient& rest);
void sync_spot_clock(VenueState& state, net::RestClient& rest);

} // namespace raikiri::venues::mexc::detail
