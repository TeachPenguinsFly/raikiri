#pragma once

// internal namespace: an anonymous one in a header is a distinct type per file

#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/kraken/book.hpp>
#include <raikiri/venues/kraken/errors.hpp>
#include <raikiri/venues/kraken/map.hpp>
#include <raikiri/venues/kraken/rest.hpp>
#include <raikiri/venues/kraken/spot.hpp>
#include <raikiri/venues/kraken/ws.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::kraken::internal {

// kraken pushes the whole book, so the 1 mib default gets headroom
constexpr std::size_t k_max_message_bytes = std::size_t{4} * 1024 * 1024;

// the memo is the only source of the instrument for a removal delta
constexpr std::size_t k_max_live_orders = 8'192;

// a removal frame carries only order_id, is_cancel and reason
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

// single threaded: the gateway loop is the only caller
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag, MarketType market_type)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag}, market{market_type} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    exec::RateBudget* budget{nullptr};
    bool testnet;
    // spot is market data only, on its own hosts
    MarketType market;
    std::uint64_t order_answer_timeout_ns{0};

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;

    net::Signer signer;

    std::vector<OwnedSubscription> desired;
    std::vector<OwnedSubscription> unsent;
    std::unordered_map<std::uint64_t, Book> books;
    std::unordered_map<std::uint64_t, SpotBook> spot_books;
    std::unordered_map<std::string, LiveOrder> live_orders;

    std::int64_t clock_offset_ns{0};

    std::uint64_t subscribe_refusals{0};

    [[nodiscard]] bool spot() const noexcept { return market == MarketType::Spot; }

    [[nodiscard]] UnixNanos venue_now() const noexcept {
        const auto local = clock->wall();
        return UnixNanos{static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    void remember(std::string_view order_id, const LiveOrder& order) {
        if (order_id.empty()) {
            return;
        }
        const std::string key{order_id};
        if (live_orders.size() >= k_max_live_orders && !live_orders.contains(key)) {
            return; // bounded so the memo cannot exhaust memory
        }
        live_orders[key] = order;
    }

    [[nodiscard]] const LiveOrder* recall(std::string_view order_id) const {
        const auto it = live_orders.find(std::string{order_id});
        return it == live_orders.end() ? nullptr : &it->second;
    }

    Book& book_for(const InstrumentBase& base) {
        return books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size).first->second;
    }

    SpotBook& spot_book_for(const InstrumentBase& base) {
        return spot_books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size, k_spot_book_depth)
            .first->second;
    }

    void add_subscription(std::string_view feed, std::string_view product_id) {
        kraken::add_subscription(desired, feed, product_id);
    }
};

inline std::optional<net::RestResponse> await_response(VenueState& state,
                                                       net::RestClient& rest,
                                                       const net::RestRequest& request,
                                                       std::uint64_t timeout_ms = 20'000) {
    return net::await_response(state.io, *state.clock, rest, request, timeout_ms);
}

} // namespace raikiri::venues::kraken::internal
