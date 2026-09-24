#pragma once

// every depth frame is a complete top-n snapshot; there is no delta chain

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bingx {

// T is when the book changed, E when bingx sent it
struct WireBookTicker {
    std::string_view symbol;
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    // swap only; zero on spot
    std::uint64_t update_id{0};
    std::uint64_t transaction_ms{0};
    std::uint64_t event_ms{0};
};

[[nodiscard]] std::expected<WireBookTicker, WireError> parse_book_ticker(std::string_view frame);

struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

inline constexpr std::size_t k_max_levels = 128;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// spot sends asks descending and swap ascending; both sides come out best first
struct WireDepth {
    std::string_view symbol;
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids; // descending: best bid first
    std::span<const WireLevel> asks; // ascending: best ask first
};

[[nodiscard]] std::expected<WireDepth, WireError>
parse_depth(std::string_view frame, LevelBuffer& storage, MarketType market);

} // namespace raikiri::venues::bingx
