#pragma once

// What the venue says this account is long or short.

#include <raikiri/venues/kraken/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::kraken {

// GET /openpositions. `price` is a full-precision average entry that no tick
// size divides — see map.cpp for what that costs.
struct WirePosition {
    std::string_view symbol;
    std::string_view price;
    std::string_view size;
    bool is_long{false};
};

inline constexpr std::size_t k_max_positions = 512;

struct PositionBuffer {
    std::array<WirePosition, k_max_positions> positions{};
};

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view body, PositionBuffer& storage);

} // namespace raikiri::venues::kraken
