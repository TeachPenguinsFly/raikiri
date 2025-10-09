#pragma once

// Coinbase Exchange error classification.
//
// The generic gateway retries Retryable, backs off RateLimited, halts on
// AuthFailure and synthesizes a terminal Rejected for Terminal — so a
// condition in the wrong bucket is a trading bug, not a logging detail.
//
// Unrecognised conditions default to Terminal, not Retryable: an unknown
// refusal repeated at full speed is how an IP earns a ban, while a spuriously
// terminal request is recoverable by asking again deliberately.

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/error.hpp>

#include <string_view>

namespace raikiri::venues::coinbase {

// Every parser in this venue reports failure with this. It lives beside the
// status vocabulary because both answer one question: what did the venue say
// went wrong.
using WireError = wire::Error;

// 200 and 404 were observed live (404 on GET /products/NOPE-XYZ, body
// {"message":"NotFound"}); the rest are the documented contract. 429 could
// not be provoked from a public endpoint at 60 concurrent requests, so its
// bucket is from the docs rather than from a capture.
[[nodiscard]] exec::ErrorClass classify_http(int status) noexcept;

// Websocket failures, which carry no status either. "Failed to subscribe" is
// the observed text for a bad product id and is terminal for that
// subscription — asking again produces the same refusal.
[[nodiscard]] exec::ErrorClass classify_stream(std::string_view message) noexcept;

} // namespace raikiri::venues::coinbase
