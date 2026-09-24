#pragma once

#include <raikiri/wire/error.hpp>

#include <cstdint>

namespace raikiri::venues::phemex {

using WireError = wire::Error;

// answered per subscription past the socket's ceiling; the socket stays up
inline constexpr std::int64_t k_ws_subscription_limit = 6015;

} // namespace raikiri::venues::phemex
