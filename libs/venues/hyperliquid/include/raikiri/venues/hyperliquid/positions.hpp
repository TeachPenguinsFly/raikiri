#pragma once

// What the venue says this account is long or short.

#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::hyperliquid {

// A row of clearinghouseState.assetPositions[].position.
//
// `szi` is SIGNED — positive long, negative short — and there is no side
// field to disagree with it. It is also serialized from a float: a real
// response carries "-4216984.4000000004" for a position whose lot size is
// 0.1, so a reader that demands an exact multiple of the step will refuse
// rows the venue considers ordinary.
struct WirePosition {
    std::string_view coin;
    std::string_view size;     // szi
    std::string_view entry_px; // a weighted average, not a book price
};

inline constexpr std::size_t k_max_positions = 256;

struct PositionBuffer {
    std::array<WirePosition, k_max_positions> positions{};
    // clearinghouseState.time — the instant the whole snapshot describes.
    // There is no per-position timestamp, so every row shares this one.
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view body, PositionBuffer& storage);

} // namespace raikiri::venues::hyperliquid
