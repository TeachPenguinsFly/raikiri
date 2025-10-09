#pragma once

// MEXC error-code classification.
//
// The generic gateway retries Retryable, backs off RateLimited, halts on
// AuthFailure and synthesizes a terminal Rejected for Terminal — so a code in
// the wrong bucket is a trading bug, not a logging detail.
//
// Unrecognised codes default to Terminal, not Retryable: an unknown refusal
// repeated at full speed is how an account earns an IP ban, while a spuriously
// terminal order is recoverable by reconciliation.
//
// Marked VERIFIED codes were provoked against production; the rest are from
// the API reference and stay provisional until seen.

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/error.hpp>

#include <cstdint>

namespace raikiri::venues::mexc {

// Every parser in this venue reports failure with this. It lives beside the
// code vocabulary because both answer one question: what did the venue say
// went wrong.
using WireError = wire::Error;

// Futures codes. Non-negative; zero means success.
inline constexpr std::int64_t k_not_logged_in = 401; // VERIFIED: no credentials
inline constexpr std::int64_t k_key_expired = 402;   // VERIFIED 2026-08-26: bogus ApiKey
inline constexpr std::int64_t k_ip_not_whitelisted = 406;
inline constexpr std::int64_t k_internal_error = 500; // VERIFIED
inline constexpr std::int64_t k_system_busy = 501;
inline constexpr std::int64_t k_unknown_source = 506;
inline constexpr std::int64_t k_rate_limited = 510; // VERIFIED: 60-request burst
inline constexpr std::int64_t k_endpoint_unreachable = 511;
// 513, not 603: the clock-skew refusal is "invalid request (open api serves
// time more or less than 10 s)". 603 is a DUPLICATE request and retrying one
// of those is how the same order gets placed twice.
inline constexpr std::int64_t k_request_expired = 513;
inline constexpr std::int64_t k_parameter_error = 600; // VERIFIED: limit=abc
inline constexpr std::int64_t k_decode_error = 601;
inline constexpr std::int64_t k_signature_invalid = 602;
inline constexpr std::int64_t k_repeated_request = 603;
inline constexpr std::int64_t k_permission_read = 701;
inline constexpr std::int64_t k_permission_modify = 702;
inline constexpr std::int64_t k_permission_trade_read = 703;
inline constexpr std::int64_t k_permission_trade_modify = 704;
inline constexpr std::int64_t k_account_not_found = 1000;
inline constexpr std::int64_t k_contract_not_found = 1001; // VERIFIED: NOSUCH_USDT
inline constexpr std::int64_t k_contract_not_activated = 1002;
inline constexpr std::int64_t k_amount_error = 1004;
inline constexpr std::int64_t k_order_direction_invalid = 2001;
inline constexpr std::int64_t k_insufficient_balance = 2005;
inline constexpr std::int64_t k_leverage_error = 2006;
inline constexpr std::int64_t k_order_quantity_error = 2011;
inline constexpr std::int64_t k_cancel_over_limit = 2013;
inline constexpr std::int64_t k_client_order_id_too_long = 2030;
// Per-order verdicts inside a cancel response, not envelope codes: 2040 is an
// order MEXC no longer knows and 2041 one it has already finished with.
inline constexpr std::int64_t k_cancel_order_not_exist = 2040;
inline constexpr std::int64_t k_cancel_order_state = 2041;
inline constexpr std::int64_t k_trading_forbidden = 6001;
inline constexpr std::int64_t k_open_forbidden = 6002;

[[nodiscard]] exec::ErrorClass classify(std::int64_t code) noexcept;

// Spot codes live in a SEPARATE namespace that overlaps the futures one
// numerically: 401 is "not logged in" on the futures tree and a bare HTTP
// status on /api/v3, and 500 means different things on each. One shared table
// would put half of these in the wrong bucket, which is a trading bug rather
// than a logging detail — hence two functions.
inline constexpr std::int64_t k_spot_bad_request = 400;
inline constexpr std::int64_t k_spot_unauthorized = 401;
inline constexpr std::int64_t k_spot_forbidden = 403;
// VERIFIED 2026-08-26: a signed endpoint reached WITHOUT timestamp+signature
// answers 404 rather than a parameter error, so a build bug reads as a routing
// bug.
inline constexpr std::int64_t k_spot_not_found = 404;
inline constexpr std::int64_t k_spot_rate_limited = 429;
inline constexpr std::int64_t k_spot_internal = 500;
inline constexpr std::int64_t k_spot_symbol_not_api = 10007;
inline constexpr std::int64_t k_spot_api_key_invalid = 10072; // VERIFIED: bogus key
inline constexpr std::int64_t k_spot_insufficient_balance = 10101;
inline constexpr std::int64_t k_spot_trading_disabled = 30016;
inline constexpr std::int64_t k_spot_order_not_exist = 30025;
inline constexpr std::int64_t k_spot_key_missing = 700001;
inline constexpr std::int64_t k_spot_signature_invalid = 700002;
inline constexpr std::int64_t k_spot_request_expired = 700003;
inline constexpr std::int64_t k_spot_order_target_missing = 700004;
inline constexpr std::int64_t k_spot_recv_window_too_large = 700005;
inline constexpr std::int64_t k_spot_ip_not_whitelisted = 700006;
inline constexpr std::int64_t k_spot_no_permission = 700007;

[[nodiscard]] exec::ErrorClass classify_spot(std::int64_t code) noexcept;

[[nodiscard]] bool is_benign_spot_cancel_failure(std::int64_t code) noexcept;

// A cancel for an order MEXC has already finished with usually means the order
// filled or was already cancelled; reconciliation settles the truth. Callers
// use this to avoid an alarming CancelRejected for a benign race.
[[nodiscard]] bool is_benign_cancel_failure(std::int64_t code) noexcept;

} // namespace raikiri::venues::mexc
