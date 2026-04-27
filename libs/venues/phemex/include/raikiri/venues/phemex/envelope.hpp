#pragma once

#include <raikiri/venues/phemex/errors.hpp>

#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::phemex {

enum class EventKind : std::uint8_t {
    RealBook,      // orderbook_p
    ScaledBook,    // book
    RealTrades,    // trades_p
    ScaledTrades,  // trades
    PerpPack,      // perp_market24h_pack_p
    InverseTicker, // market24h
    SpotTicker,    // spot_market24h
    Reply,         // a subscription ack or a pong
    Error,
    Unknown,
};

[[nodiscard]] std::expected<EventKind, WireError> parse_event(std::string_view frame);

struct StreamError {
    std::int64_t code{0};
    std::string_view message;
    std::uint64_t id{0};
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

} // namespace raikiri::venues::phemex
