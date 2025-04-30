#pragma once

// MEXC futures error-code classification.
//
// The generic gateway retries Retryable, backs off RateLimited, halts on
// AuthFailure and synthesizes a terminal Rejected for Terminal — so a code in
// the wrong bucket is a trading bug, not a logging detail.
//
// Unrecognised codes default to Terminal, not Retryable: an unknown refusal
// repeated at full speed is how an account earns an IP ban, while a spuriously
// terminal order is recoverable by reconciliation.
//
// Marked VERIFIED codes were provoked against production on 2026-08-10; the
// rest are from the contract API reference and stay provisional until seen.

#include <raikiri/exec/error.hpp>

#include <cstdint>

namespace raikiri::venues::mexc {

// MEXC codes are non-negative; zero means success.
inline constexpr std::int64_t k_not_logged_in = 401;   // VERIFIED: no credentials
inline constexpr std::int64_t k_internal_error = 500;  // VERIFIED
inline constexpr std::int64_t k_rate_limited = 510;    // VERIFIED: 60-request burst
inline constexpr std::int64_t k_parameter_error = 600; // VERIFIED: limit=abc
inline constexpr std::int64_t k_signature_invalid = 602;
inline constexpr std::int64_t k_request_expired = 603; // Request-Time outside the window
inline constexpr std::int64_t k_ip_not_whitelisted = 606;
inline constexpr std::int64_t k_account_not_found = 1000;
inline constexpr std::int64_t k_contract_not_found = 1001; // VERIFIED: NOSUCH_USDT
inline constexpr std::int64_t k_contract_not_activated = 1002;
inline constexpr std::int64_t k_system_busy = 1004;
inline constexpr std::int64_t k_order_direction_invalid = 2001;
inline constexpr std::int64_t k_insufficient_balance = 2005;
inline constexpr std::int64_t k_order_count_over_limit = 2011;
inline constexpr std::int64_t k_cancel_order_not_exist = 2013;
inline constexpr std::int64_t k_client_order_id_too_long = 2030;
inline constexpr std::int64_t k_trading_forbidden = 30016;
inline constexpr std::int64_t k_insufficient_position = 30004;

[[nodiscard]] exec::ErrorClass classify(std::int64_t code) noexcept;

// A cancel for an order MEXC has already finished with usually means the order
// filled or was already cancelled; reconciliation settles the truth. Callers
// use this to avoid an alarming CancelRejected for a benign race.
[[nodiscard]] bool is_benign_cancel_failure(std::int64_t code) noexcept;

} // namespace raikiri::venues::mexc
