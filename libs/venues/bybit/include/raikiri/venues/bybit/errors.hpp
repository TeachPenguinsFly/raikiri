#pragma once

// Bybit retCode classification.
//
// This mapping is the difference between a gateway that recovers and one
// that hammers a venue or, worse, quietly abandons an order. The generic
// gateway retries Retryable, backs off RateLimited, halts on AuthFailure,
// and synthesizes a terminal Rejected event for Terminal — so putting a code
// in the wrong bucket is a trading bug, not a logging detail.
//
// The default for an unrecognised code is Terminal rather than Retryable: an
// unknown refusal repeated at full speed is how an account gets rate-limited
// into a ban, and a spuriously terminal order is recoverable by
// reconciliation while a retry storm is not.

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/error.hpp>

#include <cstdint>

namespace raikiri::venues::bybit {

// Every parser in this venue reports failure with this. It lives beside the
// retCode vocabulary because both answer one question: what did the venue
// say went wrong.
using WireError = wire::Error;

// Codes that matter enough to name. Everything else falls through to the
// classifier's default.
inline constexpr std::int64_t k_ret_ok = 0;
inline constexpr std::int64_t k_ret_param_error = 10001;
inline constexpr std::int64_t k_ret_invalid_api_key = 10003;
inline constexpr std::int64_t k_ret_sign_error = 10004;
inline constexpr std::int64_t k_ret_permission_denied = 10005;
inline constexpr std::int64_t k_ret_rate_limit = 10006;
inline constexpr std::int64_t k_ret_ip_banned = 10010;
inline constexpr std::int64_t k_ret_timestamp_error = 10002; // recv_window skew
inline constexpr std::int64_t k_ret_server_timeout = 10016;
inline constexpr std::int64_t k_ret_order_not_exists = 110001;
inline constexpr std::int64_t k_ret_insufficient_balance = 110007;
inline constexpr std::int64_t k_ret_qty_too_small = 110017;
inline constexpr std::int64_t k_ret_price_out_of_range = 110094;
inline constexpr std::int64_t k_ret_duplicate_order_link_id = 110072;
inline constexpr std::int64_t k_ret_reduce_only_rejected = 110043;
inline constexpr std::int64_t k_ret_too_many_open_orders = 110070;

[[nodiscard]] exec::ErrorClass classify(std::int64_t ret_code) noexcept;

// A cancel for an order the venue has never heard of is not a failure worth
// retrying — it usually means the order already filled or was already
// cancelled, and the reconciliation pull will settle the truth. Callers use
// this to avoid emitting an alarming CancelRejected for a benign race.
[[nodiscard]] bool is_benign_cancel_failure(std::int64_t ret_code) noexcept;

} // namespace raikiri::venues::bybit
