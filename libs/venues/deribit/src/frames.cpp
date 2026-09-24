#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/deribit/book.hpp>
#include <raikiri/venues/deribit/envelope.hpp>
#include <raikiri/venues/deribit/errors.hpp>
#include <raikiri/venues/deribit/map.hpp>
#include <raikiri/venues/deribit/rest.hpp>
#include <raikiri/venues/deribit/ws.hpp>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "state.hpp"

namespace raikiri::venues::deribit::internal {

namespace {

void handle_book(VenueState& state, std::string_view frame, UnixNanos received) {
    static thread_local LevelBuffer levels;
    const auto wire = parse_book(frame, levels);
    if (!wire.has_value()) {
        return;
    }
    const auto* instrument = state.instruments.find(wire->instrument);
    if (instrument == nullptr) {
        return;
    }
    const auto& base = base_of(*instrument);
    auto& book = state.book_for(base);
    const auto result = book.apply(*wire);
    if (result != BookResult::Applied && result != BookResult::Resynced) {
        return; // unsynced, redelivered or dropped
    }
    if (const auto depth = book.snapshot(ms_to_unix(wire->ts_ms, received), received);
        depth.has_value()) {
        state.sink->on_depth(*depth);
    }
}

// the order row beside the trade is the authority; otherwise only filled names a quantity
std::optional<QtyLots> leaves_for(const WireUserTrade& trade,
                                  std::span<const WireOrder> orders,
                                  const exec::InstrumentTable& table) {
    for (const auto& order : orders) {
        if (order.order_id == trade.order_id) {
            return order_leaves(order, table);
        }
    }
    return trade.state == "filled" ? std::optional{QtyLots::from_raw(0)} : std::nullopt;
}

// fills go first so an order that finished in the same frame is already accounted for
void handle_changes(VenueState& state, std::string_view frame, UnixNanos received) {
    static thread_local OrderBuffer orders;
    static thread_local UserTradeBuffer trades;
    const auto wire = parse_user_changes(frame, orders, trades);
    if (!wire.has_value()) {
        return;
    }
    for (const auto& trade : wire->trades) {
        const auto leaves = leaves_for(trade, wire->orders, state.instruments);
        if (!leaves.has_value()) {
            continue;
        }
        if (const auto event = map_fill(trade, state.instruments, *leaves, received);
            event.has_value()) {
            state.sink->on_order_event(*event);
        }
    }
    for (const auto& order : wire->orders) {
        if (const auto event = map_order_event(order, state.instruments, received);
            event.has_value()) {
            state.sink->on_order_event(*event);
        }
    }
}

} // namespace

bool handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        return false; // unknown shapes are ignored, never fatal
    }
    switch (event->kind) {
    case EventKind::Bbo: {
        const auto wire = parse_bbo(frame);
        if (!wire.has_value()) {
            return false;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        break;
    }
    case EventKind::Trade: {
        static thread_local TradeBuffer trades;
        const auto wire = parse_trades(frame, trades);
        if (!wire.has_value()) {
            return false;
        }
        for (const auto& entry : *wire) {
            if (const auto trade = map_trade(entry, state.instruments, received);
                trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case EventKind::Ticker: {
        const auto wire = parse_ticker(frame);
        if (!wire.has_value()) {
            return false;
        }
        if (const auto funding = map_funding(*wire, state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        if (const auto greeks = map_greeks(*wire, state.instruments, received);
            greeks.has_value()) {
            state.sink->on_option_greeks(*greeks);
        }
        if (const auto stats = map_ticker_stats(*wire, state.instruments, received);
            stats.has_value()) {
            state.sink->on_ticker_stats(*stats);
        }
        break;
    }
    case EventKind::Book:
        handle_book(state, frame, received);
        break;
    case EventKind::UserChange:
        handle_changes(state, frame, received);
        break;
    case EventKind::Heartbeat:
        // an unanswered test_request costs the connection
        return event->needs_test_reply;
    case EventKind::Response: {
        const auto response = parse_response(frame);
        if (!response.has_value()) {
            return false;
        }
        if (response->failed()) {
            state.last_error_code = response->error_code;
            break;
        }
        state.channels_accepted += response->accepted;
        break;
    }
    case EventKind::Unknown:
        break;
    }
    return false;
}

} // namespace raikiri::venues::deribit::internal
