#pragma once

// KuCoin error-code classification, spot and futures alike.
//
// The generic gateway retries Retryable, backs off RateLimited, halts on
// AuthFailure and synthesizes a terminal Rejected for Terminal — so a code in
// the wrong bucket is a trading bug, not a logging detail.
//
// Unrecognised codes default to Terminal, not Retryable: an unknown refusal
// repeated at full speed is how an account earns an IP ban, while a spuriously
// terminal order is recoverable by reconciliation.

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/json.hpp>

#include <cstdint>
#include <string_view>

namespace raikiri::venues::kucoin {

// Every parser in this venue reports failure with this. It lives beside the
// code vocabulary because both answer one question: what did the venue say
// went wrong.
using WireError = wire::Error;

// REST. 200000 is success; k_ok in envelope.hpp is the same value.
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
inline constexpr std::int64_t k_network_timeout = 408000;
inline constexpr std::int64_t k_user_frozen = 411100;
inline constexpr std::int64_t k_unsupported = 415000; // VERIFIED live: no funding rate
inline constexpr std::int64_t k_too_many_requests = 429000;
inline constexpr std::int64_t k_internal_server_error = 500000;
inline constexpr std::int64_t k_service_unavailable = 503000;

// Order entry. KuCoin publishes a SEPARATE table per product, and the two
// overlap in neither numbering nor meaning — the spot codes below come from
// the spot table, the futures ones from the futures table.
inline constexpr std::int64_t k_spot_order_count_exceeded = 200003;
inline constexpr std::int64_t k_spot_balance_insufficient = 200004;
inline constexpr std::int64_t k_spot_symbol_not_tradable = 600203;
inline constexpr std::int64_t k_spot_symbol_unknown = 900001;

inline constexpr std::int64_t k_futures_order_not_cancelable = 100004;
inline constexpr std::int64_t k_futures_balance_insufficient = 300003;
inline constexpr std::int64_t k_futures_no_position_to_close = 300009;
inline constexpr std::int64_t k_futures_leverage_too_high = 300016;
inline constexpr std::int64_t k_futures_client_oid_repeated = 300018;

// Websocket.
inline constexpr std::int64_t k_ws_token_invalid = 401;   // VERIFIED live
inline constexpr std::int64_t k_ws_topic_not_found = 404; // VERIFIED live

[[nodiscard]] exec::ErrorClass classify(std::int64_t code) noexcept;

// An expired bullet token needs a new token, not a halted venue. The caller
// reruns the handshake instead of waiting for the socket's own backoff, which
// would reconnect with the same dead token forever.
[[nodiscard]] bool is_expired_token(std::int64_t code) noexcept;

// A cancel that lost the race with a fill, or with an earlier cancel, asked
// for a state the order is already in — reporting CancelRejected there would
// resurrect an order the OMS has correctly written off. KuCoin says so with a
// generic 400100 whose TEXT is the only discriminator, so the text is read
// here rather than the code alone.
[[nodiscard]] bool is_benign_cancel_failure(std::int64_t code, std::string_view msg) noexcept;

} // namespace raikiri::venues::kucoin
