#pragma once

// frames arrive on channel 0 tagged ps/pn/pu/pc; rest returns the same arrays

#include <raikiri/venues/bitfinex/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitfinex {

// amount's sign is the position: negative is short
inline constexpr std::size_t k_position_arity = 14;
inline constexpr std::size_t k_position_symbol = 0;
inline constexpr std::size_t k_position_status = 1;
inline constexpr std::size_t k_position_amount = 2;
inline constexpr std::size_t k_position_base_price = 3;
inline constexpr std::size_t k_position_pl = 6;
inline constexpr std::size_t k_position_liq_price = 8;
inline constexpr std::size_t k_position_leverage = 9;
inline constexpr std::size_t k_position_id = 11;
inline constexpr std::size_t k_position_mts_create = 12;
inline constexpr std::size_t k_position_mts_update = 13;

struct WirePosition {
    std::string_view symbol;
    std::string_view status; // "ACTIVE" | "CLOSED"
    std::string_view amount; // signed; negative is short
    std::string_view base_price;
    std::uint64_t mts_update{0};
};

inline constexpr std::size_t k_max_positions = 512;

struct PositionBuffer {
    std::array<WirePosition, k_max_positions> positions{};
};

[[nodiscard]] std::expected<WirePosition, WireError> parse_position(std::string_view frame);

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                                      PositionBuffer& storage);

} // namespace raikiri::venues::bitfinex
