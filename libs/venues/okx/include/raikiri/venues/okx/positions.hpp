#pragma once

#include <raikiri/common/time.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::okx {

// pos is signed in net mode and per side in long/short mode
struct WirePosition {
    std::string_view inst_id;
    std::string_view pos;
    std::string_view pos_side; // "net" | "long" | "short"
    std::string_view avg_px;
    std::uint64_t u_ms{0};
};

inline constexpr std::size_t k_max_positions = 256;

struct PositionBuffer {
    std::array<WirePosition, k_max_positions> positions{};
};

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                                      PositionBuffer& storage);

} // namespace raikiri::venues::okx
