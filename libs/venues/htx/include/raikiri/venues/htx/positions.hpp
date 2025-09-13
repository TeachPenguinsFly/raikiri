#pragma once

// What the venue says this account is long or short. Swap only: HTX's spot
// tree has no position concept.

#include <raikiri/venues/htx/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::htx {

struct WirePosition {
    std::string_view contract_code;
    std::string_view volume; // MAGNITUDE in contracts; `sell` carries the sign
    std::string_view cost_hold;
    bool sell{false};
    std::uint64_t ts_ms{0};
};

// swap_cross_position_info answers every open contract in one body. The same
// value as the order ceiling and deliberately a separate constant: one buffer
// sized by another domain's limit resizes silently when that limit moves.
inline constexpr std::size_t k_max_positions = 512;

struct PositionBuffer {
    std::array<WirePosition, k_max_positions> positions{};
};

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view body, PositionBuffer& storage);

} // namespace raikiri::venues::htx
