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
// THE THING TO KNOW ABOUT THIS VENUE: a refusal arrives as HTTP 200 with the
// verdict in the body. `{"code":100413,...}` — a dead API key — comes back
// with a 200 status (VERIFIED against production, unauthenticated, on every
// private path this adapter uses), so reading the status code instead of the
// envelope calls a revoked key a healthy request. The one exception is
// /openApi/user/auth/userDataStream, which answers in HTTP statuses and no
// envelope at all: 200 with the key, 204 when no key header was sent, and 404
// for a key BingX does not know (VERIFIED the same way).
//
// The codes below are BingX's own published reference plus the ones this
// adapter provoked against production. The bucketing is the judgement call:
// the clock-skew refusal is Retryable because resyncing fixes it, and the
// permission refusals are AuthFailure because retrying one at full speed is
// how a restricted key becomes a banned IP.

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/error.hpp>

#include <cstdint>

namespace raikiri::venues::bingx {

// Every parser in this venue reports failure with this. It lives beside the
// code vocabulary because both answer one question: what did the venue say
// went wrong.
using WireError = wire::Error;

// A websocket subscription refusal. An unlisted symbol and an unknown dataType
// share this code, so a refusal cannot be diagnosed more precisely than "that
// stream will never arrive".
inline constexpr std::int64_t k_ws_data_type_unsupported = 80015;

// The signature did not verify: the digest, the parameter string or the secret
// disagree. Not retryable — the same request signs the same way.
inline constexpr std::int64_t k_bad_signature = 100001;

// The key is valid and lacks the permission the endpoint needs.
inline constexpr std::int64_t k_no_permission = 100004;

// "this api is not exist" — the one shape that omits `data` for `timestamp`.
inline constexpr std::int64_t k_endpoint_not_found = 100400;

// Spot's "order does not exist": a cancel that lost the race with a fill.
inline constexpr std::int64_t k_spot_order_not_found = 100404;

inline constexpr std::int64_t k_rate_limited = 100410;

inline constexpr std::int64_t k_missing_signature = 100412;

// Missing or wrong X-BX-APIKEY, one code for both, so it says nothing about the
// signature.
inline constexpr std::int64_t k_bad_api_key = 100413;

inline constexpr std::int64_t k_ip_not_whitelisted = 100419;

// Null timestamp, or one outside recvWindow of BingX's clock. Retryable on
// purpose: the adapter resyncs its offset and the next attempt lands inside
// the window, where halting a venue whose keys are fine would not.
inline constexpr std::int64_t k_bad_timestamp = 100421;

inline constexpr std::int64_t k_system_busy = 100500;

inline constexpr std::int64_t k_spot_account_restricted = 100441;

// Funding settlement is running; the order is refused for a few seconds only.
inline constexpr std::int64_t k_funding_settling = 101214;
inline constexpr std::int64_t k_assets_processing = 101219;
inline constexpr std::int64_t k_account_changed = 101412;
inline constexpr std::int64_t k_region_restricted = 101210;
inline constexpr std::int64_t k_kyc_required = 101483;
inline constexpr std::int64_t k_region_or_kyc = 101484;

// Answers /quote/depth when `limit` is not one of 5/10/20/50/100/500/1000, and
// an order whose symbol, precision or timestamp does not validate.
inline constexpr std::int64_t k_invalid_parameter = 109400;

// Risk control: the request rate or the cancel-to-fill ratio tripped a limit.
inline constexpr std::int64_t k_risk_control = 109403;
inline constexpr std::int64_t k_too_many_invalid = 109429;

// The perpetual tree's "order does not exist": filled, cancelled, or a bad id.
inline constexpr std::int64_t k_order_not_found = 109421;

inline constexpr std::int64_t k_symbol_not_found = 109425;

inline constexpr std::int64_t k_internal_error = 109500;
inline constexpr std::int64_t k_network_exception = 109701;
inline constexpr std::int64_t k_order_system_busy = 110500;

[[nodiscard]] exec::ErrorClass classify(std::int64_t code) noexcept;

// A cancel that lost a race is not an incident: the order finished before the
// cancel landed, and reconciliation already knows how that ends. Emitting a
// CancelRejected for it would ask the OMS to resume an order that is gone.
[[nodiscard]] bool is_benign_cancel_failure(std::int64_t code) noexcept;

// The rate-limit signal that needs no code table: BingX answers a throttled
// caller with HTTP 429 and an overloaded one with 5xx, both before any envelope
// exists to read.
[[nodiscard]] exec::ErrorClass classify_http(int status) noexcept;

} // namespace raikiri::venues::bingx
