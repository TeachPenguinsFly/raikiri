#pragma once

// What the venue says this account is long or short.

#include <raikiri/common/time.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::okx {

// One element of GET /api/v5/account/positions. `pos` is signed in net mode
// and unsigned-per-side in long/short mode, so pos_side has to be read: a
// short read as a long is a position error of twice the size.
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

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view body, PositionBuffer& storage);

} // namespace raikiri::venues::okx
