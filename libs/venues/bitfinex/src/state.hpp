#pragma once

// a named namespace: an anonymous one in a header is a distinct type per translation unit

#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bitfinex/book.hpp>
#include <raikiri/venues/bitfinex/errors.hpp>
#include <raikiri/venues/bitfinex/map.hpp>
#include <raikiri/venues/bitfinex/orders.hpp>
#include <raikiri/venues/bitfinex/rest.hpp>
#include <raikiri/venues/bitfinex/sign.hpp>
#include <raikiri/venues/bitfinex/ws.hpp>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::bitfinex::internal {

// heartbeats arrive about every 15 s once subscribed; this covers the gap before
constexpr std::uint64_t k_app_ping_interval_ns = 20'000'000'000ULL;

constexpr std::size_t k_max_live_orders = 8'192;

// owned symbol: the ack's buffer is gone by the first data frame
struct OwnedSubscription {
    ChannelKind kind{ChannelKind::Unknown};
    SymbolText symbol{};
};

struct ChannelBinding {
    std::int64_t chan_id{0};
    ChannelKind kind{ChannelKind::Unknown};
    SymbolText symbol{};
};

// one registry per connection: channel ids come from the server and can collide across sockets
struct ChannelRegistry {
    std::vector<ChannelBinding> entries;

    [[nodiscard]] std::span<const ChannelBinding> bound() const noexcept { return entries; }

    void bind(std::int64_t chan_id, ChannelKind kind, SymbolText symbol) {
        const ChannelBinding entry{.chan_id = chan_id, .kind = kind, .symbol = symbol};
        for (auto& existing : entries) {
            if (existing.chan_id == chan_id) {
                existing = entry;
                return;
            }
        }
        entries.push_back(entry);
    }

    [[nodiscard]] const ChannelBinding* find(std::int64_t chan_id) const noexcept {
        for (const auto& existing : bound()) {
            if (existing.chan_id == chan_id) {
                return &existing;
            }
        }
        return nullptr;
    }

    // ids are reissued on every open and after the coded info notices
    void clear() noexcept { entries.clear(); }
};

// single threaded: the gateway loop is the only caller
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref) : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    exec::RateBudget* budget{nullptr};
    std::uint64_t order_answer_timeout_ns{0};

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;

    // every key asked for across the pool's sockets
    std::vector<OwnedSubscription> desired;

    std::unordered_map<std::uint64_t, Book> books;

    std::uint64_t next_app_ping_ns{0};

    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};
    std::uint64_t unbound_frames{0};

    config::Secret api_secret;
    std::string api_key;

    // registered here because ExecutionClient has no poll hook
    std::function<void(UnixNanos)> private_poll;

    // keyed by venue order id; executions carry no client id, so fills are attributed from this
    std::unordered_map<std::string, OrderContext> live_orders;

    // our id by derived cid; the only route back from cid to ulid
    std::unordered_map<std::int64_t, ExchangeClientOrderId> by_cid;

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

    [[nodiscard]] OrderContext context_for(std::string_view order_id) const {
        const auto it = live_orders.find(std::string{order_id});
        return it == live_orders.end() ? OrderContext{} : it->second;
    }

    void remember_cid(std::int64_t cid, const ExchangeClientOrderId& id) {
        if (by_cid.size() >= k_max_live_orders && !by_cid.contains(cid)) {
            return;
        }
        by_cid[cid] = id;
    }

    [[nodiscard]] ExchangeClientOrderId client_id_for(const WireOrder& order) const {
        const auto memo = context_for(order.id);
        if (!memo.client_order_id.empty()) {
            return memo.client_order_id;
        }
        const auto it = by_cid.find(order.cid);
        return it == by_cid.end() ? ExchangeClientOrderId{} : it->second;
    }

    // false when already asked for; refcounting lives in the gateway core
    [[nodiscard]] bool add_subscription(ChannelKind kind, SymbolText symbol) {
        for (const auto& existing : desired) {
            if (existing.kind == kind && existing.symbol == symbol) {
                return false;
            }
        }
        desired.push_back(OwnedSubscription{.kind = kind, .symbol = symbol});
        return true;
    }

    Book& book_for(const InstrumentBase& base) {
        return books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size).first->second;
    }
};

inline std::optional<net::RestResponse> await_response(VenueState& state,
                                                       net::RestClient& rest,
                                                       const net::RestRequest& request,
                                                       std::uint64_t timeout_ms = 10'000) {
    return net::await_response(state.io, *state.clock, rest, request, timeout_ms);
}

} // namespace raikiri::venues::bitfinex::internal
