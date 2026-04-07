#pragma once

// What every frame on the socket is wrapped in.
//
// A data frame names itself twice: `type` says what happened
// ("subscribed/order_book" is the snapshot, "update/order_book" a delta) and
// `channel` says where, as "order_book:1" — a colon, where the subscription
// asked for "order_book/1". A refusal has neither: it is a bare
// {"error":{"code":..,"message":..}}.

#include <raikiri/venues/lighter/errors.hpp>

#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::lighter {

enum class EventKind : std::uint8_t {
    OrderBook,
    Trade,
    MarketStats,     // perps: mark, funding, 24h
    SpotMarketStats, // spot: 24h only
    Connected,
    Error,
    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    bool snapshot{false}; // the `subscribed/` frame, as against `update/`
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// "order_book:1" -> 1. Nullopt-shaped as an error for "market_stats:all" and
// anything else that does not end in a number.
[[nodiscard]] std::expected<std::uint32_t, WireError> channel_market(std::string_view channel);

struct StreamError {
    std::int64_t code{0};
    std::string_view message;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

} // namespace raikiri::venues::lighter
