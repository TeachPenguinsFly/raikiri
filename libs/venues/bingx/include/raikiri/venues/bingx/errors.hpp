#pragma once

// BingX error-code classification.
//
// The generic gateway retries Retryable, backs off RateLimited, halts on
// AuthFailure and synthesizes a terminal Rejected for Terminal — so a code in
// the wrong bucket is a trading bug, not a logging detail. Unrecognised codes
// default to Terminal: an unknown refusal repeated at full speed is how an
// account earns an IP ban, while a spuriously terminal order is recoverable by
// reconciliation.
//
// The table is deliberately short. BingX's published error reference is a
// JavaScript application that serves no document to fetch, so every code below
// is one this adapter provoked against production and read off the wire;
// guessing at the rest would put codes in buckets nobody has checked. When
// signing lands, the timestamp-window refusal it earns belongs in Retryable —
// clock skew is fixed by resyncing, not by halting a venue whose keys are fine.

#include <raikiri/exec/error.hpp>

#include <cstdint>

namespace raikiri::venues::bingx {

// A websocket subscription refusal. An unlisted symbol and an unknown dataType
// share this code, so a refusal cannot be diagnosed more precisely than "that
// stream will never arrive".
inline constexpr std::int64_t k_ws_data_type_unsupported = 80015;

// "this api is not exist" — the one shape that omits `data` for `timestamp`.
inline constexpr std::int64_t k_endpoint_not_found = 100400;

// Missing or wrong X-BX-APIKEY, one code for both, so it says nothing about the
// signature.
inline constexpr std::int64_t k_bad_api_key = 100413;

// Answers /quote/depth when `limit` is not one of 5/10/20/50/100/500/1000.
inline constexpr std::int64_t k_invalid_parameter = 109400;

inline constexpr std::int64_t k_symbol_not_found = 109425;

[[nodiscard]] exec::ErrorClass classify(std::int64_t code) noexcept;

// The rate-limit signal that needs no code table: BingX answers a throttled
// caller with HTTP 429 and an overloaded one with 5xx, both before any envelope
// exists to read.
[[nodiscard]] exec::ErrorClass classify_http(int status) noexcept;

} // namespace raikiri::venues::bingx
