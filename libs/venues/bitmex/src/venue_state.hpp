#pragma once

// What the three bitmex clients share, and nothing else.
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
#include <raikiri/venues/bitmex/book.hpp>
#include <raikiri/venues/bitmex/errors.hpp>
#include <raikiri/venues/bitmex/map.hpp>
#include <raikiri/venues/bitmex/rest.hpp>
#include <raikiri/venues/bitmex/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::bitmex::detail {

// A ceiling on what one process remembers about working orders. The memo is the
// ONLY thing that names an instrument when an `order` update arrives: BitMEX
// keys that table on orderID alone, and an update can carry nothing else.
constexpr std::size_t k_max_live_orders = 8'192;

// Owned as strings: the instrument table's views outlive nothing in particular.
struct OwnedSubscription {
    std::string table;
    std::string symbol;
};

// What an `order` update does not say, and what this process has already told
// the OMS about the order. `last_effect` is the deduplication key: the REST
// response and the private stream both describe the same transition, and the
// second one to arrive must stay quiet.
struct LiveOrder {
    InstrumentId instrument{};
    ExchangeClientOrderId client_order_id{};
    Side side{Side::Buy};
    OrderType order_type{OrderType::Limit};
    OrderFlags flags{OrderFlags::None};
    PriceTicks price{};
    QtyLots qty{};
    QtyLots leaves{};
    OrderEffect last_effect{OrderEffect::Silent};
};

// The per-instrument state the partial frames force, keyed by SymbolId.
struct SymbolState {
    QuoteBook quotes;
    InstrumentState instrument;
};

// State every bitmex connection shares within one gateway process. Single
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

    std::vector<SymbolState> symbols;
    std::unordered_map<std::uint64_t, Book> books;
    std::unordered_map<std::string, LiveOrder> live_orders;

    std::uint64_t next_app_ping_ns{0};

    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_status{0};

    void add_subscription(std::string_view table, std::string_view symbol) {
        for (const auto& existing : desired) {
            if (existing.table == table && existing.symbol == symbol) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{table}, std::string{symbol});
    }

    void remember(const std::string& order_id, const LiveOrder& order) {
        if (order_id.empty()) {
            return;
        }
        if (live_orders.size() >= k_max_live_orders && !live_orders.contains(order_id)) {
            return; // a bounded memo beats an unbounded one that OOMs the gateway
        }
        live_orders[order_id] = order;
    }

    [[nodiscard]] LiveOrder* recall(const std::string& order_id) {
        const auto it = live_orders.find(order_id);
        return it == live_orders.end() ? nullptr : &it->second;
    }

    [[nodiscard]] SymbolState& state_of(InstrumentId id) {
        const auto idx = id.symbol().raw;
        if (idx >= symbols.size()) {
            symbols.resize(static_cast<std::size_t>(idx) + 1);
        }
        return symbols[idx];
    }

    Book& book_for(const InstrumentBase& base) {
        return books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size)
            .first->second;
    }
};

inline std::optional<net::RestResponse> await_response(VenueState& state,
                                                       net::RestClient& rest,
                                                       const net::RestRequest& request,
                                                       std::uint64_t timeout_ms) {
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

} // namespace raikiri::venues::bitmex::detail
