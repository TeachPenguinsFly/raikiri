#pragma once

// HMAC-SHA384, which Bitfinex is the only venue here to use.
//
// libs/net/signer.hpp carries SHA-256 and SHA-512 and nothing between, and
// SHA-384 is NOT SHA-512 truncated by the caller — it is SHA-512 with a
// different initial state, so taking the first 48 bytes of a SHA-512 digest
// produces a signature Bitfinex refuses with 10100. It lives here rather than
// in libs/net because one venue needs it.
//
// Both transports sign with it and neither signs the same bytes:
//
//   REST  `/api/v2` + path + nonce + body   (the body is `{}` when empty, and
//         it is the EXACT bytes sent — a re-serialised payload is a bad
//         signature)
//   WS    `AUTH` + nonce + nonce            (the nonce appears TWICE; once is
//         answered 10100 and reads as a revoked key)
//
// VERIFIED against Bitfinex's own SDKs: bfx-api-node-util/lib/gen_auth_sig.js
// and bfx-api-node-rest/lib/rest2.js `_makeAuthRequest`.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace raikiri::venues::bitfinex {

inline constexpr std::size_t k_hmac384_bytes = 48;
inline constexpr std::size_t k_hex384_bytes = k_hmac384_bytes * 2; // 96

// Lowercase hex. `out` must hold at least k_hex384_bytes; returns the written
// view, or an empty one on an OpenSSL failure or a short buffer.
[[nodiscard]] std::string_view
hmac_sha384_hex(std::string_view key, std::string_view message, std::span<char> out) noexcept;

// The websocket auth frame, built and signed in one place because the payload
// and the digest have to agree byte for byte and the payload's shape is the
// easy half to get wrong: `AUTH` + nonce + THE SAME NONCE AGAIN.
//
// `out` receives the whole frame; returns an empty view on a short buffer or a
// missing credential. No `filter` is sent — every private stream arrives and
// the reader ignores the tags it has no channel for, which is cheaper to
// reason about than a filter whose omissions are silent.
[[nodiscard]] std::string_view ws_auth_frame(std::string_view api_key,
                                             std::string_view secret,
                                             std::uint64_t nonce,
                                             std::span<char> out) noexcept;

// Bitfinex nonces are MICROSECONDS and must strictly increase per key. The
// venue rejects a nonce that is not greater than the last one it saw (10114),
// and two requests inside the same microsecond are not hypothetical on a warm
// connection pool — so the counter, not the clock, is the source of the value.
class NonceSource {
public:
    // Never returns the same value twice, and never goes backwards, even if
    // the wall clock does.
    [[nodiscard]] std::uint64_t next(std::uint64_t wall_ns) noexcept;

private:
    std::uint64_t last_{0};
};

} // namespace raikiri::venues::bitfinex
