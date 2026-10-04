#pragma once

#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
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

namespace raikiri::venues::htx::internal {

constexpr std::size_t k_max_live_orders = 8'192;

struct OwnedSubscription {
    std::string code;
    std::string suffix;
};

// single threaded: only the gateway loop touches it
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref) : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    std::uint64_t order_answer_timeout_ns{0};

    net::IoContext io;
    net::Signer signer;
    exec::InstrumentTable instruments;

    Protocol protocol;

    std::vector<OwnedSubscription> desired;
    std::vector<std::string> funding_codes;

    std::int64_t clock_offset_ns{0};

    std::uint64_t subscribe_refusals{0};
    std::string last_error_code;

    std::uint64_t inflate_failures{0};

    // keyed by venue order id, the only field every private frame carries
    struct OrderMemo {
        ClientOrderId client_order_id{};
        ExchangeClientOrderId exchange_client_order_id{};
        QtyLots leaves{QtyLots::from_raw(0)};
    };

    std::unordered_map<std::string, OrderMemo> live_orders;

    // the swap was sent an int64 client id; this ties it to the ulid and the contract code
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

    // htx refuses a Timestamp a few minutes off its clock as a bad signature
    [[nodiscard]] UnixNanos venue_now() const noexcept {
        const auto local = clock->wall();
        return UnixNanos{static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
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
        const auto identity = map_identity(wire.client_order_id, wire.order_id, base_of(*instrument).id.market());
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
    return net::await_response(state.io, *state.clock, rest, request, timeout_ms);
}

// exhaustive so Option needs an explicit decision
inline std::optional<exec::VenueError> refuse_market(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
    case MarketType::Perp:
        return std::nullopt;
    case MarketType::Future:
        return exec::not_implemented("htx dated futures order entry");
    case MarketType::Option:
        return exec::not_implemented("htx options order entry");
    }
    return exec::not_implemented("htx: unknown market");
}

} // namespace raikiri::venues::htx::internal
