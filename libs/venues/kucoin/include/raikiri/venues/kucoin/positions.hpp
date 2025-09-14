#pragma once

// What the venue says this account is long or short.

#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::kucoin {

// `current_qty` is SIGNED and counted in contracts: negative is short, and
// there is no side field to disagree with it.
struct WirePosition {
    std::string_view symbol;
    std::string_view current_qty;
    std::string_view avg_entry_price;
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_positions = 256;

struct PositionBuffer {
    std::array<WirePosition, k_max_positions> positions{};
};

// GET /api/v1/positions — `data` is the array itself.
[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view body, PositionBuffer& storage);

} // namespace raikiri::venues::kucoin
