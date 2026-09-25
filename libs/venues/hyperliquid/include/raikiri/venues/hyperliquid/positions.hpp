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
};

inline constexpr std::size_t k_max_positions = 256;

struct PositionBuffer {
    std::array<WirePosition, k_max_positions> positions{};
    // snapshot time shared by every position row
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                                      PositionBuffer& storage);

} // namespace raikiri::venues::hyperliquid
