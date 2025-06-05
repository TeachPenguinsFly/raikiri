#pragma once

// Kraken Futures request authentication.
//
// The scheme is unlike every other venue in this tree and every step of it is
// load-bearing:
//
//     Authent = base64( HMAC-SHA512( base64_decode(secret), SHA256(message) ) )
//
// The secret is base64-DECODED before it keys the HMAC; signing with the
// printable form authenticates as a bad key, which reads like a revoked
// credential rather than a coding error. The inner digest is SHA-256 and the
// outer one HMAC-SHA-512 keyed over those 32 RAW bytes, never over their hex
// text.
//
// libs/net offers hex-encoded SHA-512 and raw HMAC-SHA-256 — neither is a step
// in this chain and no base64 decoder exists there at all — so the primitives
// live here rather than as an edit to a file sixteen adapters share.

#include <cstddef>
#include <span>
#include <string_view>

namespace raikiri::venues::kraken {

// base64 of a 64-byte digest: 88 characters including the single '=' pad.
inline constexpr std::size_t k_authent_bytes = 88;

// REST signs `postData + nonce + endpointPath`, where endpointPath is the URL
// path with the "/derivatives" prefix REMOVED — Kraken sends
// "/derivatives/api/v3/sendorder" and signs "/api/v3/sendorder". The websocket
// challenge runs the same pipeline over the challenge UUID alone, which is why
// this takes a finished message rather than the pieces.
//
// Empty on a secret that is not valid base64 or an `out` shorter than
// k_authent_bytes. An empty signature is never sent: the caller refuses.
[[nodiscard]] std::string_view
authent(std::string_view secret_base64, std::string_view message, std::span<char> out) noexcept;

} // namespace raikiri::venues::kraken
