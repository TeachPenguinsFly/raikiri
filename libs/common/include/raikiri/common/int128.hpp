#pragma once

// __int128 is a GCC/Clang extension; -Wpedantic flags it. Both production
// compilers support it on every target we build for (x86-64, arm64).

namespace raikiri {

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
using int128_t = __int128;
using uint128_t = unsigned __int128;
#pragma GCC diagnostic pop

} // namespace raikiri
