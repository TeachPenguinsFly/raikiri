#pragma once

#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::kucoin {

// signed contract count; negative is short
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

// GET /api/v1/positions: data is the array itself
[[nodiscard]] std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                                      PositionBuffer& storage);

} // namespace raikiri::venues::kucoin
