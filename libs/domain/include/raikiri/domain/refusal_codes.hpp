#pragma once

// why strategyd denied an order, in the reject code of its journalled event; negative so no venue's code is mistaken
// for one. these numbers are in postgres: add, never renumber

#include <cstdint>

namespace raikiri {

inline constexpr std::int32_t k_refused_risk = -1;
inline constexpr std::int32_t k_refused_duplicate = -2;
inline constexpr std::int32_t k_refused_venue_down = -3;
inline constexpr std::int32_t k_refused_capacity = -4;
inline constexpr std::int32_t k_refused_naked_short = -10;
inline constexpr std::int32_t k_refused_short_margin_unknown = -11;
inline constexpr std::int32_t k_refused_short_margin = -12;
inline constexpr std::int32_t k_refused_short_greeks = -13;

} // namespace raikiri
