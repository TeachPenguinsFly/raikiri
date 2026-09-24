#pragma once

// unknown codes are terminal; AuthFailure halts the venue, so per-symbol refusals stay terminal

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/error.hpp>

#include <cstdint>

namespace raikiri::venues::weex {

using WireError = wire::Error;

// weex codes are negative; zero means no error
inline constexpr std::int64_t k_ok = 0;

inline constexpr std::int64_t k_access_key_empty = -1040;
inline constexpr std::int64_t k_access_sign_empty = -1041;
inline constexpr std::int64_t k_access_timestamp_empty = -1042;
inline constexpr std::int64_t k_access_timestamp_invalid = -1043;
inline constexpr std::int64_t k_access_key_invalid = -1044;
// the 30 s clock window; reads like a revoked key
inline constexpr std::int64_t k_access_timestamp_expired = -1046;
inline constexpr std::int64_t k_auth_failed = -1047;
inline constexpr std::int64_t k_key_or_passphrase_wrong = -1049;
inline constexpr std::int64_t k_user_forbidden = -1050;
inline constexpr std::int64_t k_permission_denied = -1051;
inline constexpr std::int64_t k_insufficient_permissions = -1052;
inline constexpr std::int64_t k_permission_check_failed = -1053;
inline constexpr std::int64_t k_user_auth_not_safe = -1055;
inline constexpr std::int64_t k_illegal_ip = -1056;
inline constexpr std::int64_t k_user_locked = -1057;
inline constexpr std::int64_t k_access_forbidden = -1190;

// the only code that earns a retry
inline constexpr std::int64_t k_system_error = -1054;
inline constexpr std::int64_t k_order_rate_limited = -1059;

inline constexpr std::int64_t k_param_empty = -1141;
inline constexpr std::int64_t k_param_invalid = -1142;
inline constexpr std::int64_t k_invalid_json = -1135;

// cancel for an order the venue no longer has; benign
inline constexpr std::int64_t k_order_not_found = -3200;
// see /capi/v3/market/apiTradingSymbols for the tradable list
inline constexpr std::int64_t k_symbol_not_api_tradable = -1058;
inline constexpr std::int64_t k_key_not_bound_to_symbol = -1060;
inline constexpr std::int64_t k_contract_no_permission_pair = -3235;

[[nodiscard]] exec::ErrorClass classify(std::int64_t code) noexcept;

// nothing left to cancel counts as success
[[nodiscard]] bool is_benign_cancel_failure(std::int64_t code) noexcept;

} // namespace raikiri::venues::weex
