#pragma once

// Coinbase Exchange authentication.
//
//   prehash   = timestamp + METHOD + requestPath + body
//   signature = BASE64( HMAC-SHA256( BASE64_DECODE(secret), prehash ) )
//
// The base64 DECODE of the secret is the part that goes wrong quietly: the
// key Coinbase issues is printable text, and using it verbatim produces a
// digest of the right length and shape that the venue answers 401 to, which
// reads like a revoked key rather than a coding error. The secret is decoded
// ONCE at credential load and the Signer holds the 64 raw bytes, so the
// mistake cannot be made per request.
//
// `timestamp` is SECONDS since the epoch (decimals allowed), not the
// milliseconds every other venue here wants, and it must be within 30 seconds
// of the venue's clock.
//
// The websocket signs the same way over the fixed string
// timestamp + "GET" + "/users/self/verify", and the digest travels inside the
// subscribe message rather than in a header — there are no headers on a
// socket. One decode, one HMAC, two transports.

#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::coinbase {

// The path the websocket handshake signs instead of a real request.
inline constexpr std::string_view k_ws_signature_path = "/users/self/verify";

// A base64 secret decodes to 64 bytes; the buffer allows for a longer key
// rather than assuming the one length observed.
inline constexpr std::size_t k_max_secret_bytes = 128;

// Decodes standard base64 into `out` and returns the byte count, or nullopt
// for anything that is not base64 — which is itself the answer to "why is
// every request 401": the key was pasted wrong, not revoked.
[[nodiscard]] std::optional<std::size_t> decode_secret(std::string_view base64_secret,
                                                       std::span<char> out) noexcept;

// timestamp + METHOD + requestPath + body. Empty on overflow, because a
// signature over a short prehash authenticates a request nobody built.
[[nodiscard]] std::string_view prehash(std::string_view timestamp,
                                       net::HttpMethod method,
                                       std::string_view request_path,
                                       std::string_view body,
                                       std::span<char> out) noexcept;

// Whole seconds. Coinbase accepts decimals and needs none: the window is 30
// seconds wide, so sub-second precision buys nothing and costs a formatter.
[[nodiscard]] std::string_view timestamp_s(UnixNanos now, std::span<char> out) noexcept;

} // namespace raikiri::venues::coinbase
