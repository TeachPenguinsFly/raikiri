#pragma once

#include <raikiri/wire/error.hpp>

#include <cstdint>

namespace raikiri::venues::lighter {

using WireError = wire::Error;

// An error frame names no channel, so a refusal can only be counted.
inline constexpr std::int64_t k_ws_invalid_channel = 30005;
inline constexpr std::int64_t k_ws_fetch_failed = 30004; // e.g. market_stats on a spot id
inline constexpr std::int64_t k_ws_already_subscribed = 30003;
inline constexpr std::int64_t k_ws_rate_limit = 30009;
inline constexpr std::int64_t k_ws_too_many_inflight = 30010;
inline constexpr std::int64_t k_too_many_subscriptions = 23001;

} // namespace raikiri::venues::lighter
