#pragma once

#include <raikiri/venues/bitget/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitget {

// holdSide names the direction; no signed size
struct WirePosition {
    std::string_view symbol; // `instId` on the socket, `symbol` over REST
    std::string_view hold_side;
    std::string_view total;
    std::string_view open_price_avg;
    std::uint64_t update_ms{0};
};

inline constexpr std::size_t k_max_positions = 128;

struct PositionBuffer {
    std::array<WirePosition, k_max_positions> positions{};
};

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view body, PositionBuffer& storage);

} // namespace raikiri::venues::bitget
