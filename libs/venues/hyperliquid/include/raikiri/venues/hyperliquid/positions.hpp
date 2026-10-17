#pragma once

#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::hyperliquid {

// `szi` is signed and float-serialised, so it may not be an exact multiple of the lot
struct WirePosition {
    std::string_view coin;
    std::string_view size;     // szi
    std::string_view entry_px; // a weighted average, not a book price
    std::string_view unrealized_pnl;
};

// accountValue counts open pnl; withdrawable is what the margin in use leaves free
struct WireMarginSummary {
    std::string_view account_value;
    std::string_view withdrawable;
};

inline constexpr std::size_t k_max_positions = 256;

struct PositionBuffer {
    std::array<WirePosition, k_max_positions> positions{};
    // empty where the answer leaves the summary out
    WireMarginSummary margin{};
    // snapshot time shared by every position row
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                                      PositionBuffer& storage);

} // namespace raikiri::venues::hyperliquid
