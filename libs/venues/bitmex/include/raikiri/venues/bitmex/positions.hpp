#pragma once

// What the venue says this account is long or short.
//
// The `position` table streams the same row REST answers, and the
// partial-column rule holds here too: an update carries only what moved.

#include <raikiri/venues/bitmex/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitmex {

// `currentQty` is SIGNED and counted in the venue's own units; there is no side
// field that could disagree with it, and no separate long and short row.
struct WirePosition {
    std::string_view symbol;
    std::string_view current_qty;
    std::string_view avg_entry_price;
    std::string_view timestamp;
};

inline constexpr std::size_t k_max_positions = 512;

struct PositionBuffer {
    std::array<WirePosition, k_max_positions> positions{};
};

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view payload, PositionBuffer& storage);

} // namespace raikiri::venues::bitmex
