#pragma once

// What the venue says went wrong, and which of those this adapter can see.
//
// Only the stream's codes are listed: the adapter sends nothing but
// subscriptions, and the REST call it makes has no error body worth a code.

#include <raikiri/wire/error.hpp>

#include <cstdint>

namespace raikiri::venues::lighter {

using WireError = wire::Error;

// {"error":{"code":30009,"message":"Too Many Websocket Messages!"}}, and the
// frame names NO channel — a refusal cannot be traced back to the
// subscription it refused, only counted.
inline constexpr std::int64_t k_ws_invalid_channel = 30005;
inline constexpr std::int64_t k_ws_fetch_failed = 30004; // e.g. market_stats on a spot id
inline constexpr std::int64_t k_ws_already_subscribed = 30003;
inline constexpr std::int64_t k_ws_rate_limit = 30009;
inline constexpr std::int64_t k_ws_too_many_inflight = 30010;
inline constexpr std::int64_t k_too_many_subscriptions = 23001;

} // namespace raikiri::venues::lighter
