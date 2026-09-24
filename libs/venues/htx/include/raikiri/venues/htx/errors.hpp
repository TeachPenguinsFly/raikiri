#pragma once

// unknown codes default to Terminal so an unknown refusal is not retried at full speed

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/error.hpp>

#include <cstdint>
#include <string_view>

namespace raikiri::venues::htx {

using WireError = wire::Error;

// a missing SignatureMethod and a bad AccessKeyId both answer 403
inline constexpr std::int64_t k_signature_rejected = 403;

inline constexpr std::int64_t k_contract_not_found = 1014;
inline constexpr std::int64_t k_parameter_empty = 1066;
inline constexpr std::int64_t k_parameter_illegal = 1067;

inline constexpr std::int64_t k_swap_not_found = 1332;

inline constexpr std::int64_t k_notify_contract_not_found = 2011;

// market socket's refusal for an unparseable topic
inline constexpr std::string_view k_ws_bad_request = "bad-request";
inline constexpr std::string_view k_invalid_parameter = "invalid-parameter";

// match the integer code; the err_msg text differs from the docs
inline constexpr std::int64_t k_swap_query_error = 1002;
inline constexpr std::int64_t k_swap_redis_error = 1003;
inline constexpr std::int64_t k_swap_system_busy = 1004;
inline constexpr std::int64_t k_swap_contract_missing = 1014;

// htx's faq names 1032 as the derivative rate limit
inline constexpr std::int64_t k_swap_rate_limited = 1032;

inline constexpr std::int64_t k_swap_service_abnormal = 1046;
inline constexpr std::int64_t k_swap_insufficient_margin = 1047;
inline constexpr std::int64_t k_swap_nothing_to_cancel = 1051;
inline constexpr std::int64_t k_swap_order_missing = 1061;
inline constexpr std::int64_t k_swap_cancel_in_flight = 1062;
inline constexpr std::int64_t k_swap_already_executed = 1063;

// the swap types client_order_id as int64
inline constexpr std::int64_t k_swap_client_id_not_integer = 1065;

inline constexpr std::int64_t k_swap_already_cancelled = 1071;

// over 3000 limit/post_only/ioc/fok orders in 10 min at >99% cancel ratio bans those types for 5 min
inline constexpr std::int64_t k_swap_cancel_ratio_ban = 1084;

inline constexpr std::int64_t k_swap_service_unavailable = 1108;

inline constexpr std::int64_t k_swap_auth_detail_first = 12'001;
inline constexpr std::int64_t k_swap_auth_detail_last = 12'009;

// spot answers login-required for a missing key or signature
inline constexpr std::string_view k_spot_login_required = "login-required";
inline constexpr std::string_view k_spot_signature_invalid = "api-signature-not-valid";

// cancel of an order that already finished; reconciliation settles it
inline constexpr std::string_view k_spot_order_state = "order-orderstate-error";
inline constexpr std::string_view k_spot_record_invalid = "base-record-invalid";
inline constexpr std::string_view k_spot_not_found = "not-found";
inline constexpr std::string_view k_spot_base_not_found = "base-not-found";

// minted here: cancel-by-client-order-id answers state 0 for an unknown id
inline constexpr std::string_view k_client_id_not_found = "client-order-id-not-found";

inline constexpr std::int64_t k_too_many_requests = 429;

[[nodiscard]] exec::ErrorClass classify(std::string_view code) noexcept;

// the order is already gone, so this must not become a CancelRejected
[[nodiscard]] bool is_benign_cancel_failure(std::string_view code) noexcept;

[[nodiscard]] exec::ErrorClass classify_http(int status) noexcept;

} // namespace raikiri::venues::htx
