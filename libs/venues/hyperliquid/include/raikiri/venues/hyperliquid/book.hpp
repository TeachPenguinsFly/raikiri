#pragma once

// every l2Book frame is a full snapshot, so there is no sequence to track

#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::hyperliquid {

struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

struct WireBbo {
    std::string_view coin;
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::uint64_t ts_ms{0};
};

// a side with nothing resting is null and yields empty views
[[nodiscard]] std::expected<WireBbo, WireError> parse_bbo(std::string_view frame);

inline constexpr std::size_t k_max_levels = 256;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// [bids, asks], bids descending and asks ascending
struct WireBook {
    std::string_view coin;
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBook, WireError> parse_book(std::string_view frame,
                                                            LevelBuffer& storage);

} // namespace raikiri::venues::hyperliquid
