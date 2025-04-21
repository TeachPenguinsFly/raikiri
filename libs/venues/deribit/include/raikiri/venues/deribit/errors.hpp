#pragma once

// Deribit error-code classification.
//
// The generic gateway retries Retryable, backs off RateLimited and halts on
// AuthFailure, so a code in the wrong bucket is an operational bug.
// Unrecognised codes default to Terminal: an unknown refusal repeated at full
// speed is how an IP earns a ban.
//
// Two code spaces share one field. JSON-RPC's own errors are NEGATIVE (-32602,
// -32601 — both verified live); Deribit's own are positive. Zero is an error
// code on neither side, which is why WireResponse uses a sentinel instead.

#include <raikiri/exec/error.hpp>

#include <cstdint>

namespace raikiri::venues::deribit {

// JSON-RPC 2.0 transport errors.
inline constexpr std::int64_t k_parse_error = -32700;
inline constexpr std::int64_t k_request_too_large = -32600;
inline constexpr std::int64_t k_method_not_found = -32601; // VERIFIED live
inline constexpr std::int64_t k_invalid_params = -32602;   // VERIFIED live
inline constexpr std::int64_t k_missing_params = -32000;

// Deribit application errors.
inline constexpr std::int64_t k_authorization_required = 10000;
inline constexpr std::int64_t k_invalid_credentials_v1 = 10001;
inline constexpr std::int64_t k_qty_too_low = 10002;
inline constexpr std::int64_t k_order_not_found = 10004;
inline constexpr std::int64_t k_not_enough_funds = 10009;
inline constexpr std::int64_t k_already_closed = 10010;
inline constexpr std::int64_t k_too_many_requests = 10028;
inline constexpr std::int64_t k_retry = 10040;
inline constexpr std::int64_t k_settlement_in_progress = 10041;
inline constexpr std::int64_t k_matching_engine_queue_full = 10047;
inline constexpr std::int64_t k_too_many_concurrent_requests = 10066;
inline constexpr std::int64_t k_already_filled = 11008;
inline constexpr std::int64_t k_system_maintenance = 11051;
inline constexpr std::int64_t k_subscribe_error_unsubscribed = 11052;
inline constexpr std::int64_t k_invalid_credentials = 13004;
inline constexpr std::int64_t k_unauthorized = 13009;
inline constexpr std::int64_t k_forbidden = 13021;
inline constexpr std::int64_t k_temporarily_unavailable = 13028;
inline constexpr std::int64_t k_unavailable = 13503;
// VERIFIED live: an unauthenticated socket asking for a "raw" channel — a
// permanent property of our credentials, and it took the whole subscribe
// batch with it, which is why ws.hpp keeps batches small.
inline constexpr std::int64_t k_raw_subscriptions_unauthorized = 13778;

[[nodiscard]] exec::ErrorClass classify(std::int64_t code) noexcept;

} // namespace raikiri::venues::deribit
