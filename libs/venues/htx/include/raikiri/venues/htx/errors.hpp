#pragma once

// HTX error classification.
//
// The generic gateway retries Retryable, backs off RateLimited, halts on
// AuthFailure and synthesizes a terminal Rejected for Terminal — so a code in
// the wrong bucket is a trading bug, not a logging detail. Unrecognised codes
// default to Terminal: an unknown refusal repeated at full speed is how an
// account earns an IP ban, while a spuriously terminal order is recoverable
// by reconciliation.
//
// NOT YET KNOWN, and load-bearing when signing lands: HTX checks the access
// key before the timestamp, so the window refusal cannot be provoked without
// credentials. Whatever its code is, it belongs in Retryable — clock skew is
// fixed by resyncing, not by halting a venue whose keys are fine.

#include <raikiri/exec/error.hpp>

#include <cstdint>
#include <string_view>

namespace raikiri::venues::htx {

// The whole signature/key family: a missing SignatureMethod and a bogus
// AccessKeyId both answer 403, differing only in message.
inline constexpr std::int64_t k_signature_rejected = 403;

inline constexpr std::int64_t k_contract_not_found = 1014;
inline constexpr std::int64_t k_parameter_empty = 1066;
inline constexpr std::int64_t k_parameter_illegal = 1067;

// The swap-specific spelling of "no such contract": a different endpoint
// family answering the same mistake.
inline constexpr std::int64_t k_swap_not_found = 1332;

inline constexpr std::int64_t k_notify_contract_not_found = 2011;

// The market socket's refusal for a topic it cannot parse, and
// linear-swap-ex's for a bad query parameter. Words, not numbers.
inline constexpr std::string_view k_ws_bad_request = "bad-request";
inline constexpr std::string_view k_invalid_parameter = "invalid-parameter";

[[nodiscard]] exec::ErrorClass classify(std::string_view code) noexcept;

// The signals that need no code table: HTX answers a throttled caller with
// HTTP 429 and an unrouted path with a Spring 404 carrying no envelope.
[[nodiscard]] exec::ErrorClass classify_http(int status) noexcept;

} // namespace raikiri::venues::htx
