#pragma once

// Coinbase Exchange error classification.
//
// The generic gateway retries Retryable, backs off RateLimited, halts on
// AuthFailure and synthesizes a terminal Rejected for Terminal — so a
// condition in the wrong bucket is a trading bug, not a logging detail.
//
// The structural difference from every other adapter here: Coinbase publishes
// NO numeric error codes. A REST failure is a bare {"message":"NotFound"} and
// the only machine-readable part is the HTTP status; a websocket failure is
// {"type":"error","message":..,"reason":..} with nothing numeric at all. So
// classification takes a status or a message instead of a code, and
// VenueError::venue_code stays 0 for this venue throughout.
//
// Unrecognised conditions default to Terminal, not Retryable: an unknown
// refusal repeated at full speed is how an IP earns a ban, while a spuriously
// terminal request is recoverable by asking again deliberately.

#include <raikiri/exec/error.hpp>

#include <string_view>

namespace raikiri::venues::coinbase {

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
