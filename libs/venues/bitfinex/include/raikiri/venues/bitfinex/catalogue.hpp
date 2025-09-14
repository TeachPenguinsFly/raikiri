#pragma once

// What the venue lists, and the numbers that decide whether an order it
// accepts is an order it will fill.

#include <raikiri/venues/bitfinex/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitfinex {

// `GET /v2/conf/pub:info:pair` answers `[[[PAIR,[..]],...]]` — three deep,
// because /conf takes a list of keys and returns one result per key.
//
// The inner row is
//   [null,null,null,MIN_ORDER_SIZE,MAX_ORDER_SIZE,null,null,null,
//    INITIAL_MARGIN,MIN_MARGIN,(null,null)]
// with arity 12 on spot and 10 on futures, and MIN/MAX_ORDER_SIZE arriving as
// STRINGS while the margin fields beside them are bare numbers. Mixed types in
// one positional array; the raw-token reader is indifferent to which.
//
// NOTE WHAT IS ABSENT: no price tick, no amount step, no status field. See
// map.hpp — the steps are derived, and there is nothing to filter on but the
// symbol itself.
inline constexpr std::size_t k_pair_row_arity = 5;
inline constexpr std::size_t k_pair_min_order_size = 3;
inline constexpr std::size_t k_pair_max_order_size = 4;

struct WirePairInfo {
    std::string_view pair; // "BTCUSD" — NO `t` prefix on this endpoint
    std::string_view min_order_size;
    std::string_view max_order_size;
};

inline constexpr std::size_t k_max_pairs = 1024;

struct PairBuffer {
    std::array<WirePairInfo, k_max_pairs> pairs{};
};

[[nodiscard]] std::expected<std::span<const WirePairInfo>, WireError>
parse_pair_info(std::string_view body, PairBuffer& storage);

} // namespace raikiri::venues::bitfinex
