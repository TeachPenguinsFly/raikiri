#pragma once

// `internal` because an anonymous namespace in a header makes a distinct type per includer

#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/mexc/envelope.hpp>
#include <raikiri/venues/mexc/errors.hpp>
#include <raikiri/venues/mexc/map.hpp>
#include <raikiri/venues/mexc/orders.hpp>
#include <raikiri/venues/mexc/rest.hpp>
#include <raikiri/venues/mexc/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::mexc::internal {

// caps memory if the venue misbehaves
constexpr std::size_t k_max_live_orders = 8'192;

struct OwnedSubscription {
    std::string channel;
    std::string symbol;
    std::uint32_t limit{0};
};

// single threaded; only the gateway loop calls in
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref)
        : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    exec::RateBudget* budget{nullptr};
    std::uint64_t order_answer_timeout_ns{0};

    // futures is json and spot protobuf; the trees share nothing
    MarketType market{MarketType::Perp};

    net::IoContext io;
    net::Signer signer;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;

    std::vector<OwnedSubscription> desired;

    std::uint64_t next_app_ping_ns{0};

    // registered so the data client's ping timer also reaches this socket
    net::WsClient* private_client{nullptr};

    // one offset per tree since each checks against its own clock
    std::int64_t clock_offset_ns{0};
    std::int64_t spot_clock_offset_ns{0};

    std::uint64_t subscribe_refusals{0};

    // the deal push lacks leaves and externalOid, so they are remembered
    // from the order push by venue order id
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

    // the caller names the tree whose clock stamped the timestamp
    [[nodiscard]] UnixNanos venue_now(MarketType tree) const noexcept {
        const auto offset = tree == MarketType::Spot ? spot_clock_offset_ns : clock_offset_ns;
        const auto local = clock->wall();
        return UnixNanos{static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + offset)};
    }

    void remember(const WireFuturesOrder& wire) {
        if (wire.order_id.empty()) {
            return;
        }
        // 3 filled, 4 cancelled, 5 invalid
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
    return net::await_response(state.io, *state.clock, rest, request, timeout_ms);
}

// futures wraps a refusal in {"success":false,"code","message"} and spot answers {"code","msg"}
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

// a skewed timestamp reads to the venue as a bad signature
void sync_clock(VenueState& state, net::RestClient& rest);
void sync_spot_clock(VenueState& state, net::RestClient& rest);

} // namespace raikiri::venues::mexc::internal
