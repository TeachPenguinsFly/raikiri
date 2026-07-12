#pragma once

// `detail`, not an anonymous namespace: a type declared in a header inside an
// anonymous namespace is a DIFFERENT type in every file that includes it.

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

namespace raikiri::venues::bitfinex::detail {

// Bitfinex heartbeats every subscribed channel about every 15 s, so a socket
// carrying subscriptions is never silent. This covers the window before the
// first one binds.
constexpr std::uint64_t k_app_ping_interval_ns = 20'000'000'000ULL;

// Live orders tracked for their leaves and our own id. Far above any sane
// working set; the cap exists so a venue incident cannot turn the map into the
// heap.
constexpr std::size_t k_max_live_orders = 8'192;

// A subscription the gateway asked for. The symbol is OWNED, not viewed: the
// instrument table's views outlive nothing in particular, and an ack's buffer
// is gone by the time the first data frame arrives. SymbolText is the domain's
// own inline carrier for exactly this, so nothing here reaches the heap.
struct OwnedSubscription {
    ChannelKind kind{ChannelKind::Unknown};
    SymbolText symbol{};
};

// What a channel id turns out to carry.
struct ChannelBinding {
    std::int64_t chan_id{0};
    ChannelKind kind{ChannelKind::Unknown};
    SymbolText symbol{};
};

// THE CHANNEL IDS OF ONE CONNECTION, AND OF NO OTHER.
//
// Bitfinex answers a subscription with a number and keys every later data
// frame by that number alone — no symbol, no channel name — so this map is
// what turns a frame into an instrument. The number belongs to the SERVER the
// socket landed on rather than to the socket: MEASURED 2026-09-08 over
// eighteen simultaneous connections, two of them shared a server and were
// handed 215923..215925 and 215926..215940 out of one sequence, while sockets
// on other servers drew blocks as close as 329 apart from a range spanning
// 109k to 288k. Nothing keeps two of those counters from meeting.
//
// So one registry for a pool of sockets is a registry that will one day answer
// a book frame with another instrument's name. One per connection, and the
// handler that owns it is its only reader.
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

    // Ids are reissued on every open of this socket and after the coded `info`
    // notices, and the ids that stood before describe nothing now.
    void clear() noexcept { entries.clear(); }
};

// State every bitfinex connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, which is why
// nothing here synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref)
        : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    exec::RateBudget* budget{nullptr};
    std::uint64_t order_answer_timeout_ns{0};

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;

    // Every key asked for ACROSS the pool's sockets, which is the set that
    // answers "have we asked for this already" and nothing else. It was an
    // array of thirty — one socket's worth — and that one number refused 173
    // of bitfinex's 183 spot pairs a subscription.
    std::vector<OwnedSubscription> desired;

    std::unordered_map<std::uint64_t, Book> books;

    std::uint64_t next_app_ping_ns{0};

    // Subscriptions the venue refused.
    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};
    std::uint64_t unbound_frames{0};

    config::Secret api_secret;
    std::string api_key;

    // The execution client's per-tick work, registered here because
    // ExecutionClient has no poll hook of its own.
    std::function<void(UnixNanos)> private_poll;

    // As much of a live order as Bitfinex states in any one frame. The
    // execution rows carry no client order id at all, so a fill can only be
    // attributed out of this. Keyed by the venue order id, the one field every
    // private frame does carry.
    std::unordered_map<std::string, OrderContext> live_orders;

    // Our own id against the `cid` we derived from it. The derivation is one
    // way — 128 bits do not fit in a day-scoped integer — so this map is the
    // only route back, and it is what lets an order Bitfinex echoes before the
    // REST answer lands still name the ULID that placed it.
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

    // False when the key is already asked for: refcounting lives in the gateway
    // core, and this is a set.
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
        return books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size)
            .first->second;
    }
};

inline std::optional<net::RestResponse> await_response(VenueState& state,
                                                       net::RestClient& rest,
                                                       const net::RestRequest& request,
                                                       std::uint64_t timeout_ms = 10'000) {
    return net::await_response(state.io, *state.clock, rest, request, timeout_ms);
}

} // namespace raikiri::venues::bitfinex::detail
