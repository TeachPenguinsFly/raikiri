#pragma once

#include <cstddef>

namespace raikiri {

inline constexpr std::size_t k_cache_line = 64;

// 128 because apple silicon fetches 128-byte lines and intel prefetches adjacent pairs
inline constexpr std::size_t k_pad_align = 128;

} // namespace raikiri
