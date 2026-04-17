#pragma once

#include <raikiri/wire/error.hpp>

#include <cstdint>

namespace raikiri::venues::blofin {

using WireError = wire::Error;

inline constexpr std::int64_t k_ok = 0;
// One bad argument refuses the whole subscribe frame, and the refusal names no `arg`.
inline constexpr std::int64_t k_ws_invalid_request = 60012;

} // namespace raikiri::venues::blofin
