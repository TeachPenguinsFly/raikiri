#pragma once

// BitMEX error classification.
//
// The generic gateway retries Retryable, backs off RateLimited, halts on
// AuthFailure and synthesizes a terminal Rejected for Terminal — so a refusal
// in the wrong bucket is a trading bug, not a logging detail.
//
// Unrecognised refusals default to Terminal, not Retryable: an unknown refusal
// repeated at full speed is how an IP earns a ban, while a spuriously terminal
// order is recoverable by reconciliation.
//
// BitMEX has NO numeric error codes: a failure is an HTTP status plus
// {"error":{message,name}}, and the name is coarse enough that "HTTPError"
// covers both 403 and 404 (verified live), so the status decides and the name
// only refines. The websocket refusal carries the same status vocabulary in a
// JSON field, which is why one function reads both.
//
// The 429 answer carries Retry-After. net::RestResponse surfaces only status
// and body, so that hint cannot be honoured and backoff falls to the gateway's
// generic RateLimited policy — recorded rather than pretended.

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/error.hpp>

#include <cstdint>
#include <string_view>

namespace raikiri::venues::bitmex {

// Every parser in this venue reports failure with this. It lives beside the
// status vocabulary because both answer one question: what did the venue say
// went wrong.
using WireError = wire::Error;

inline constexpr std::int64_t k_ok = 200;
inline constexpr std::int64_t k_bad_request = 400; // ValidationError, VERIFIED live
inline constexpr std::int64_t k_unauthorized = 401;
inline constexpr std::int64_t k_forbidden = 403; // HTTPError, VERIFIED live
inline constexpr std::int64_t k_not_found = 404; // HTTPError, VERIFIED live
inline constexpr std::int64_t k_request_timeout = 408;
inline constexpr std::int64_t k_too_many_requests = 429; // budget is per minute
inline constexpr std::int64_t k_internal_error = 500;
inline constexpr std::int64_t k_bad_gateway = 502;
inline constexpr std::int64_t k_service_unavailable = 503; // "system is overloaded"
inline constexpr std::int64_t k_gateway_timeout = 504;

// `name` may be empty: a websocket refusal has no name field, and a transport
// failure has no body to read one from.
[[nodiscard]] exec::ErrorClass classify(std::int64_t status, std::string_view name) noexcept;

} // namespace raikiri::venues::bitmex
