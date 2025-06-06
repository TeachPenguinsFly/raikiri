#pragma once

// The one string Deribit signs, shared by both authenticated transports.
//
//     StringToSign = timestamp \n nonce \n data
//     signature    = LOWERCASE HEX( HMAC-SHA256(client_secret, StringToSign) )
//
// The WebSocket's public/auth grant_type=client_signature leaves `data` empty.
// REST fills it with the request itself:
//
//     data = METHOD \n uri \n body \n
//
// where `uri` is the path WITH its query string and the trailing newline is
// present even for the empty body of a GET. One formula, two callers — which
// is why this lives beside neither rest.cpp nor ws.cpp.
//
// The timestamp is milliseconds and Deribit rejects one older than 60 seconds,
// so it comes from the venue-corrected clock, not the raw local one.

#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace raikiri::venues::deribit::sign {

// Widest is "deri-hmac-sha256 id=<128>,ts=<13>,nonce=<16>,sig=<64>".
inline constexpr std::size_t k_max_auth_header = 288;

// METHOD \n uri \n body \n — Deribit's `data` term for a REST request.
// Empty when it did not fit: a truncated request signs bytes we never send.
[[nodiscard]] std::string_view request_data(net::HttpMethod method,
                                            std::string_view uri,
                                            std::string_view body,
                                            std::span<char> out) noexcept;

// timestamp \n nonce \n data. Empty on overflow, for the same reason.
[[nodiscard]] std::string_view message(std::uint64_t timestamp_ms,
                                       std::string_view nonce,
                                       std::string_view data,
                                       std::span<char> out) noexcept;

// A single-use initialisation vector. Deribit only requires that it not
// repeat, so this is a counter rendered in hex beside the timestamp — not
// randomness, and deliberately not pretending to be.
[[nodiscard]] std::string_view nonce(std::uint64_t counter, std::span<char> out) noexcept;

// "deri-hmac-sha256 id=...,ts=...,nonce=...,sig=..." — the whole
// Authorization header value. Empty when the signer holds no credentials.
[[nodiscard]] std::string_view auth_header(const net::Signer& signer,
                                           std::uint64_t timestamp_ms,
                                           std::string_view nonce,
                                           std::string_view signature,
                                           std::span<char> out) noexcept;

} // namespace raikiri::venues::deribit::sign
