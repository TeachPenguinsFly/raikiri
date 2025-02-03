#pragma once

#include <cstddef>

namespace raikiri {

// 64 bytes on x86-64 and most ARM server cores. Shared-memory cell types
// (e.g. one BBO cell) are laid out to this.
inline constexpr std::size_t k_cache_line = 64;

// Padding boundary for independently-written hot atomics (ring heads/tails,
// heartbeats). 128 rather than 64: Apple Silicon fetches 128-byte lines and
// Intel's L2 spatial prefetcher pairs adjacent 64-byte lines, so two writers
// 64 bytes apart can still false-share. Costs a little space, never wrong.
inline constexpr std::size_t k_pad_align = 128;

} // namespace raikiri
