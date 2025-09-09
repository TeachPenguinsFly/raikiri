#pragma once

// What the three kraken clients share, and nothing else.
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
#include <raikiri/venues/kraken/book.hpp>
#include <raikiri/venues/kraken/errors.hpp>
#include <raikiri/venues/kraken/map.hpp>
#include <raikiri/venues/kraken/rest.hpp>
#include <raikiri/venues/kraken/ws.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::kraken::detail {

// Kraken pushes the whole book, not a fixed depth: a live PF_XBTUSD snapshot
// measured 107 KiB, so the transport's 1 MiB default gets headroom.
constexpr std::size_t k_max_message_bytes = std::size_t{4} * 1024 * 1024;

// A ceiling on what one process remembers about working orders. The memo is
// the ONLY thing that names an instrument when a removal delta arrives: that
// frame carries an order id and nothing else.
constexpr std::size_t k_max_live_orders = 8'192;

// A subscription the gateway asked for, owned as strings because the
// instrument table's views outlive nothing in particular.
struct OwnedSubscription {
    std::string feed;
    std::string product_id;
};

// What an order removal frame does not say. `open_orders` reports a departure
// as {"order_id":..., "is_cancel":true, "reason":...} — no instrument, no
// side, no size — so an adapter that did not write this down cannot emit the
// event at all.
struct LiveOrder {
    InstrumentId instrument{};
    ExchangeClientOrderId client_order_id{};
    Side side{Side::Buy};
    OrderType order_type{OrderType::Limit};
    OrderFlags flags{OrderFlags::None};
    PriceTicks price{};
    QtyLots qty{};
    QtyLots leaves{};
};

// State every kraken connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, which is why
// nothing here synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;
    std::uint64_t order_answer_timeout_ns{0};

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;

    net::Signer signer;

    std::vector<OwnedSubscription> desired;
    std::unordered_map<std::uint64_t, Book> books;
    std::unordered_map<std::string, LiveOrder> live_orders;

    std::int64_t clock_offset_ns{0};

    // Subscriptions the venue refused.
    std::uint64_t subscribe_refusals{0};

    [[nodiscard]] UnixNanos venue_now() const noexcept {
        const auto local = clock->wall();
        return UnixNanos{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    void remember(std::string_view order_id, const LiveOrder& order) {
        if (order_id.empty()) {
            return;
        }
        const std::string key{order_id};
        if (live_orders.size() >= k_max_live_orders && !live_orders.contains(key)) {
            return; // a bounded memo beats an unbounded one that OOMs the gateway
        }
        live_orders[key] = order;
    }

    [[nodiscard]] const LiveOrder* recall(std::string_view order_id) const {
        const auto it = live_orders.find(std::string{order_id});
        return it == live_orders.end() ? nullptr : &it->second;
    }

    Book& book_for(const InstrumentBase& base) {
        return books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size)
            .first->second;
    }

    void add_subscription(std::string_view feed, std::string_view product_id) {
        for (const auto& existing : desired) {
            if (existing.feed == feed && existing.product_id == product_id) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        const auto after_feed = std::ranges::find_if(
            desired, [&](const OwnedSubscription& entry) { return entry.feed != feed; });
        desired.insert(
            after_feed,
            OwnedSubscription{.feed = std::string{feed}, .product_id = std::string{product_id}});
    }
};

inline std::optional<net::RestResponse> await_response(VenueState& state,
                                                       net::RestClient& rest,
                                                       const net::RestRequest& request,
                                                       std::uint64_t timeout_ms = 20'000) {
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

} // namespace raikiri::venues::kraken::detail
