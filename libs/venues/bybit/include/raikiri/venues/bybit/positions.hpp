#pragma once

#include <raikiri/common/time.hpp>
#include <raikiri/venues/bybit/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bybit {

struct WirePosition {
    std::string_view symbol;
    std::string_view side; // "Buy", "Sell", or "" when flat
    std::string_view size;
    std::string_view avg_price;
    std::uint64_t updated_ms{0};
};

// sized separately from k_max_updates
inline constexpr std::size_t k_max_positions = 64;

struct PositionBuffer {
    WirePosition positions[k_max_positions];
};

// rest rows are under `list`; the socket's are under `data`
[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_position_list(std::string_view result, PositionBuffer& storage);

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view frame, PositionBuffer& storage);

} // namespace raikiri::venues::bybit
