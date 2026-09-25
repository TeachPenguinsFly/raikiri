#pragma once

#include <raikiri/venues/bitfinex/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitfinex {

// answers [[[pair,[..]],...]]; min/max order size are strings, margins numbers
// no tick, step or status: steps are derived in map.hpp
inline constexpr std::size_t k_pair_row_arity = 5;
inline constexpr std::size_t k_pair_min_order_size = 3;
inline constexpr std::size_t k_pair_max_order_size = 4;

struct WirePairInfo {
    std::string_view pair; // "BTCUSD", no t prefix here
    std::string_view min_order_size;
    std::string_view max_order_size;
};

inline constexpr std::size_t k_max_pairs = 1024;

struct PairBuffer {
    std::array<WirePairInfo, k_max_pairs> pairs{};
};

[[nodiscard]] std::expected<std::span<const WirePairInfo>, WireError> parse_pair_info(std::string_view body,
                                                                                      PairBuffer& storage);

} // namespace raikiri::venues::bitfinex
