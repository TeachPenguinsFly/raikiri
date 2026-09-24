#pragma once

// unknown codes default to terminal so an unknown refusal is not retried at full speed

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/error.hpp>

#include <cstdint>

namespace raikiri::venues::bybit {

using WireError = wire::Error;

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

// an unknown-order cancel usually means already filled or cancelled; reconciliation settles it
[[nodiscard]] bool is_benign_cancel_failure(std::int64_t ret_code) noexcept;

} // namespace raikiri::venues::bybit
