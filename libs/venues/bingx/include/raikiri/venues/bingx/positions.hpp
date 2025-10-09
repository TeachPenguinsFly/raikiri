#pragma once

// What the venue says this account is long or short. Perpetual tree only:
// BingX's spot deployment has no position concept.

#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bingx {

// `position_amt` is UNSIGNED and `position_side` carries the sign. A long and
// a short of the same size are the same number here.
struct WirePosition {
    std::string_view symbol;
    std::string_view position_side; // LONG/SHORT
    std::string_view position_amt;
    std::string_view avg_price;
    std::uint64_t update_ms{0};
};

inline constexpr std::size_t k_max_positions = 512;

struct PositionBuffer {
    std::array<WirePosition, k_max_positions> positions{};
};

// `data` is a bare array here, not an object with a list inside it.
[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view body, PositionBuffer& storage);

} // namespace raikiri::venues::bingx
