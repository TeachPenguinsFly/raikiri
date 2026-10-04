#pragma once

// anonymous types in a header differ per includer

#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
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
#include <unordered_set>
#include <vector>

namespace raikiri::venues::gate::internal {

// caps the leaves map against a venue fault
constexpr std::size_t k_max_live_orders = 8'192;

// owned strings since the table's views do not outlive this
struct OwnedSubscription {
    std::string channel;
    std::string contract;
};

// single-threaded: only the gateway loop calls in
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;
    // gate counts one request pool per ip, shared with the account poller and market data
    exec::RateBudget* budget{nullptr};
    std::uint64_t order_answer_timeout_ns{0};

    // gate names a spot pair and its perp identically; this picks host, channels and frame shapes
    MarketType market{MarketType::Perp};

    net::IoContext io;
    net::Signer signer;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;

    // the futures private socket needs this first in every subscribe payload; spot does not
    std::uint64_t futures_user_id{0};

    // leaves and client id from the orders channel, keyed by venue order id, for fills that carry neither
    struct OrderMemo {
        ExchangeClientOrderId client_order_id{};
        QtyLots leaves{QtyLots::from_raw(0)};
    };

    std::unordered_map<std::string, OrderMemo> live_orders;

    // the seed has its own pool so it does not queue behind the catalogue
    net::RestClient depth_rest{io, net::RestConfig{.host = std::string{rest_host(testnet)}}};
    // gate sends only changes, so a quiet contract is seeded once over rest
    std::unordered_set<std::uint64_t> quote_seed_due;
    std::unordered_set<std::uint64_t> funding_seed_due;
    std::unordered_map<std::uint64_t, DepthBook> books;

    std::vector<OwnedSubscription> desired;

    std::uint64_t next_app_ping_ns{0};

    // Venue clock offset, measured against /api/v4/spot/time.
    std::int64_t clock_offset_ns{0};

    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};

    // an option book ticker says nothing until the top moves, so the 1 s ticker quotes it until then
    std::unordered_set<std::uint64_t> option_quote_due;

    // per SymbolId: 1 quoted two-sided, 2 greeks, 4 traded; logged once a minute on an option gateway
    std::vector<std::uint8_t> option_coverage;
    std::uint64_t next_coverage_ns{0};

    void cover(InstrumentId id, std::uint8_t bit) {
        if (id.market() != MarketType::Option) {
            return;
        }
        const auto index = id.symbol().raw;
        if (option_coverage.size() <= index) {
            option_coverage.resize(index + 1);
        }
        option_coverage[index] |= bit;
    }

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

    // gate refuses a timestamp more than 60 s from its clock
    [[nodiscard]] UnixNanos venue_now() const noexcept {
        const auto local = clock->wall();
        return UnixNanos{static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
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
        if (live_orders.size() >= k_max_live_orders && !live_orders.contains(std::string{wire.order_id})) {
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
    return net::await_response(state.io, *state.clock, rest, request, timeout_ms);
}

// market-data rest draws cost 1 per request from the shared bucket as Poll; gate meters requests per ip
inline void meter_market_data(VenueState& state, net::RestClient& rest) {
    rest.set_admit(exec::market_data_gate(state.budget, *state.clock));
}

// gate's code is a label, so it travels in the message
inline exec::VenueError refusal_of(const std::expected<RestError, WireError>& err) {
    if (!err.has_value()) {
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable refusal body");
    }
    return exec::VenueError::make(classify(err->label), 0, err->label);
}

inline exec::VenueError rest_failure(std::string_view body) {
    return refusal_of(parse_rest_error(body));
}

// measured once at startup; a skewed timestamp reads as a bad signature

} // namespace raikiri::venues::gate::internal
