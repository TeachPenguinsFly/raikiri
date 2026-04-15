#pragma once

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
    bool snapshot{false};
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// "order_book:1" -> 1: the channel has a colon where the subscription had a slash.
[[nodiscard]] std::expected<std::uint32_t, WireError> channel_market(std::string_view channel);

struct StreamError {
    std::int64_t code{0};
    std::string_view message;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

} // namespace raikiri::venues::lighter
