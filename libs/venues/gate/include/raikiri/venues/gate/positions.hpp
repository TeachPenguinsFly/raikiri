#pragma once

#include <raikiri/venues/gate/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::gate {

struct WirePosition {
    std::string_view contract;
    std::string_view size; // signed: negative is short
    std::string_view entry_price;
    std::uint64_t update_s{0};
};

inline constexpr std::size_t k_max_positions = 1024;

struct PositionBuffer {
    std::array<WirePosition, k_max_positions> positions{};
};

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view body, PositionBuffer& storage);

} // namespace raikiri::venues::gate
