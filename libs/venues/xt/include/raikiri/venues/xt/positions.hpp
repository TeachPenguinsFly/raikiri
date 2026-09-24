#pragma once

#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::xt {

// positionSize is unsigned; positionSide gives the direction
struct WireFuturesPosition {
    std::string_view symbol;
    std::string_view position_side; // LONG | SHORT
    std::string_view position_size; // contracts, unsigned
    std::string_view entry_price;
    std::string_view contract_type;
};

inline constexpr std::size_t k_max_positions = 512;

struct FuturesPositionBuffer {
    std::array<WireFuturesPosition, k_max_positions> positions{};
};

[[nodiscard]] std::expected<std::span<const WireFuturesPosition>, WireError>
parse_futures_positions(std::string_view body, FuturesPositionBuffer& storage);

} // namespace raikiri::venues::xt
