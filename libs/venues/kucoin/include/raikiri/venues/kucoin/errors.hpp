#pragma once

// KuCoin Futures error-code classification.
//
// The generic gateway retries Retryable, backs off RateLimited, halts on
// AuthFailure and synthesizes a terminal Rejected for Terminal — so a code in
// the wrong bucket is a trading bug, not a logging detail.
//
// Unrecognised codes default to Terminal, not Retryable: an unknown refusal
// repeated at full speed is how an account earns an IP ban, while a spuriously
// terminal order is recoverable by reconciliation.
//
// KuCoin's two code namespaces (six-digit REST strings, three-digit websocket
// numbers) do not overlap, so one classify() covers both — but the constants
// stay apart, because the same three digits mean different things on each side.

#include <raikiri/exec/error.hpp>

#include <cstdint>

namespace raikiri::venues::kucoin {

// REST. 200000 is success; k_ok in wire.hpp is the same value.
inline constexpr std::int64_t k_missing_credential_header = 400001;
inline constexpr std::int64_t k_timestamp_invalid = 400002; // >5 s from server time
inline constexpr std::int64_t k_api_key_not_found = 400003;
inline constexpr std::int64_t k_passphrase_error = 400004;
inline constexpr std::int64_t k_signature_error = 400005;
inline constexpr std::int64_t k_ip_not_whitelisted = 400006;
inline constexpr std::int64_t k_access_denied = 400007;
inline constexpr std::int64_t k_parameter_error = 400100;
inline constexpr std::int64_t k_order_forbidden = 400200;
inline constexpr std::int64_t k_not_logged_in = 400401;
inline constexpr std::int64_t k_validation_failed = 400600;
inline constexpr std::int64_t k_interface_deprecated = 401000;
inline constexpr std::int64_t k_url_not_found = 404000; // VERIFIED live: unknown symbol
inline constexpr std::int64_t k_user_frozen = 411100;
inline constexpr std::int64_t k_unsupported = 415000; // VERIFIED live: no funding rate
inline constexpr std::int64_t k_too_many_requests = 429000;
inline constexpr std::int64_t k_internal_server_error = 500000;
inline constexpr std::int64_t k_service_unavailable = 503000;

// Websocket.
inline constexpr std::int64_t k_ws_token_invalid = 401;   // VERIFIED live
inline constexpr std::int64_t k_ws_topic_not_found = 404; // VERIFIED live

[[nodiscard]] exec::ErrorClass classify(std::int64_t code) noexcept;

// An expired bullet token needs a new token, not a halted venue. The caller
// reruns the handshake instead of waiting for the socket's own backoff, which
// would reconnect with the same dead token forever.
[[nodiscard]] bool is_expired_token(std::int64_t code) noexcept;

} // namespace raikiri::venues::kucoin
