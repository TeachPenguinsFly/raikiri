#pragma once

// Bitfinex error-code classification.
//
// The generic gateway retries Retryable, backs off RateLimited, halts on
// AuthFailure and synthesizes a terminal Rejected for Terminal — so a code in
// the wrong bucket is a trading bug, not a logging detail.
//
// Unrecognised codes default to Terminal, not Retryable: an unknown refusal
// repeated at full speed is how an account earns an IP ban, while a spuriously
// terminal order is recoverable by reconciliation.
//
// Codes are JSON NUMBERS on both transports here, unlike okx's digit strings —
// but the transports disagree on shape rather than on type: a websocket
// refusal is {"event":"error","code":10300,...} while a REST refusal is the
// array ["error",10020,"symbol: invalid"]. One numeric namespace covers both.
//
// 10300 (websocket) and 10020 (REST) were observed live against production;
// the rest are from the v2 error reference.

#include <raikiri/exec/error.hpp>

#include <cstdint>

namespace raikiri::venues::bitfinex {

inline constexpr std::int64_t k_unknown_event = 10000;
inline constexpr std::int64_t k_unknown_pair = 10001;
inline constexpr std::int64_t k_concurrency_limit = 10008;
inline constexpr std::int64_t k_request_parameters = 10020; // VERIFIED live (REST)
inline constexpr std::int64_t k_auth_failed = 10100;
inline constexpr std::int64_t k_auth_payload = 10111;
inline constexpr std::int64_t k_auth_signature = 10112;
inline constexpr std::int64_t k_auth_encryption = 10113;
inline constexpr std::int64_t k_auth_nonce = 10114; // clock/sequence skew, not credentials
inline constexpr std::int64_t k_auth_fail = 10200;
inline constexpr std::int64_t k_subscription_failed = 10300; // VERIFIED live (websocket)
inline constexpr std::int64_t k_already_subscribed = 10301;
inline constexpr std::int64_t k_unknown_channel = 10302;
inline constexpr std::int64_t k_channel_limit = 10305;
inline constexpr std::int64_t k_unsubscription_failed = 10400;
inline constexpr std::int64_t k_not_subscribed = 10401;
inline constexpr std::int64_t k_rate_limit = 11010;
inline constexpr std::int64_t k_server_stopping = 20051;
inline constexpr std::int64_t k_maintenance_start = 20060;
inline constexpr std::int64_t k_maintenance_end = 20061;

[[nodiscard]] exec::ErrorClass classify(std::int64_t code) noexcept;

} // namespace raikiri::venues::bitfinex
