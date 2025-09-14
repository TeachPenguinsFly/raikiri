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
// XT is the only venue here whose error identity is a STRING, and it uses TWO
// DISJOINT VOCABULARIES that must not share a classifier:
//
//   futures  `error.code` beside returnCode 1 — snake_case names
//            ("invalid_symbol"), tagged names ("E000"), GATEWAY_000n, and bare
//            HTTP statuses ("400"). 403 and 405 are credential problems; 400 is
//            NOT, because XT also answers it to a blacklisted URL keyword.
//   spot     `mc` beside rc 1 — AUTH_nnn, SYMBOL_nnn, ORDER_nnn, COMMON_nnn,
//            GATEWAY_000n. The HTTP status is 200 on a refusal, so `mc` is the
//            ONLY thing that says the call failed.
//
// Everything marked VERIFIED was observed against production 2026-08; the
// tables come from XT's own reference (docs/{spot,futures}/Access Description/
// errorCode.mdx).

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/json.hpp>

#include <cstdint>
#include <string_view>

namespace raikiri::venues::xt {

// Every parser in this venue reports failure with this. It lives beside the
// code vocabulary because both answer one question: what did the venue say
// went wrong.
using WireError = wire::Error;

// VERIFIED: returned for an unknown symbol, and for an empty or absent one.
inline constexpr std::string_view k_invalid_symbol = "invalid_symbol";
// VERIFIED: a parameter that failed to parse (level=abc).
inline constexpr std::string_view k_invalid_param = "E000";
// VERIFIED 2026-08-26: every private spot endpoint answers this with no
// credentials, at HTTP 200.
inline constexpr std::string_view k_spot_missing_key = "AUTH_001";
inline constexpr std::string_view k_spot_success = "SUCCESS";

[[nodiscard]] exec::ErrorClass classify(std::string_view code) noexcept;

// Spot speaks its own vocabulary and shares not one code with futures, so one
// classifier for both would read AUTH_103 (a bad signature) as Terminal and
// leave an operator hunting a venue outage.
[[nodiscard]] exec::ErrorClass classify_spot(std::string_view code) noexcept;

// A cancel that lost a race with a fill is not a failure worth reporting: the
// order is already gone and reconciliation settles it. Only the codes that
// name a MISSING order qualify — futures `invalid_params` also covers a
// genuine parameter bug and must not be swallowed with them.
[[nodiscard]] bool is_benign_cancel_failure(std::string_view code) noexcept;

// XT's codes are text, but VenueError carries a numeric one. A bare HTTP
// status survives the trip; a named code has no integer form and reports 0,
// with the text preserved in the message.
[[nodiscard]] std::int64_t numeric_code(std::string_view code) noexcept;

} // namespace raikiri::venues::xt
