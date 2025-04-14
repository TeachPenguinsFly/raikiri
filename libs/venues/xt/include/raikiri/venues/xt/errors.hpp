#pragma once

// XT error-code classification.
//
// The generic gateway retries Retryable, backs off RateLimited, halts on
// AuthFailure and synthesizes a terminal Rejected for Terminal — so a code in
// the wrong bucket is a trading bug, not a logging detail.
//
// Unrecognised codes default to Terminal, not Retryable: an unknown refusal
// repeated at full speed is how an account earns an IP ban, while a spuriously
// terminal order is recoverable by reconciliation.
//
// XT is the only venue here whose error identity is a STRING. `returnCode`
// carries one bit (0 succeeded, 1 failed) and the code lives in error.code,
// where two disjoint families appear: snake_case names ("invalid_symbol"),
// tagged names ("E000"), and bare HTTP statuses ("400"). Everything below
// marked VERIFIED was observed against production 2026-08; the rest of the
// numeric family is inferred from the one status XT was seen to use, because
// no amount of unauthenticated probing could provoke the others — 400 requests
// at concurrency 40 drew no throttling at all, so the rate-limit code remains
// unconfirmed.

#include <raikiri/exec/error.hpp>

#include <cstdint>
#include <string_view>

namespace raikiri::venues::xt {

// VERIFIED: returned for an unknown symbol, and for an empty or absent one.
inline constexpr std::string_view k_invalid_symbol = "invalid_symbol";
// VERIFIED: a parameter that failed to parse (level=abc).
inline constexpr std::string_view k_invalid_param = "E000";

[[nodiscard]] exec::ErrorClass classify(std::string_view code) noexcept;

// XT's codes are text, but VenueError carries a numeric one. A bare HTTP
// status survives the trip; a named code has no integer form and reports 0,
// with the text preserved in the message.
[[nodiscard]] std::int64_t numeric_code(std::string_view code) noexcept;

} // namespace raikiri::venues::xt
