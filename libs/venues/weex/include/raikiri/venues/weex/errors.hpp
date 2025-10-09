#pragma once

// WEEX error-code classification.
//
// The generic gateway retries Retryable, backs off RateLimited, halts on
// AuthFailure and synthesizes a terminal Rejected for Terminal — so a code in
// the wrong bucket is a trading bug, not a logging detail.
//
// Unrecognised codes default to Terminal, not Retryable: an unknown refusal
// repeated at full speed is how an IP earns a ban, while a spuriously terminal
// read is recoverable by asking again on the next tick.
//
// The vocabulary here is SMALL ON PURPOSE. This adapter reads public market
// data and nothing else, so the only refusals it can provoke are the ones a
// malformed public request draws; every code below was read off a live 4xx on
// 2026-09-07 rather than transcribed from a page.

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/error.hpp>

#include <cstdint>

namespace raikiri::venues::weex {

// Every parser in this venue reports failure with this. It lives beside the
// code vocabulary because both answer one question: what did the venue say
// went wrong.
using WireError = wire::Error;

// WEEX codes are negative; zero means no error, which is why a body is judged
// by the SIGN of `code` rather than by the presence of the key.
inline constexpr std::int64_t k_ok = 0;

// Both measured: `GET /capi/v3/market/depth` with no symbol answered -1141 and
// with limit=9999 answered -1142, each at HTTP 400.
inline constexpr std::int64_t k_param_empty = -1141;
inline constexpr std::int64_t k_param_invalid = -1142;

[[nodiscard]] exec::ErrorClass classify(std::int64_t code) noexcept;

} // namespace raikiri::venues::weex
