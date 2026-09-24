#pragma once

// unknown codes default to terminal; json-rpc codes are negative, deribit's own positive

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/error.hpp>

#include <cstdint>

namespace raikiri::venues::deribit {

using WireError = wire::Error;

inline constexpr std::int64_t k_parse_error = -32700;
inline constexpr std::int64_t k_request_too_large = -32600;
inline constexpr std::int64_t k_method_not_found = -32601;
inline constexpr std::int64_t k_invalid_params = -32602;
inline constexpr std::int64_t k_missing_params = -32000;

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
// refused on an unauthenticated socket, and it fails the whole subscribe batch
inline constexpr std::int64_t k_raw_subscriptions_unauthorized = 13778;

inline constexpr std::int64_t k_order_overlap = 10003;
inline constexpr std::int64_t k_locked_by_admin = 10019;
inline constexpr std::int64_t k_not_owner_of_order = 10029;
inline constexpr std::int64_t k_not_open_order = 11044;
inline constexpr std::int64_t k_permission_denied = 11042;
inline constexpr std::int64_t k_account_locked = 11098;
inline constexpr std::int64_t k_internal_server_error = 11094;
inline constexpr std::int64_t k_unauthenticated_public_disabled = 11056;
inline constexpr std::int64_t k_method_switched_off = 13025;
inline constexpr std::int64_t k_scope_exceeded = 13403;
inline constexpr std::int64_t k_edit_replaced = 13777;
// the order never existed, so resending is a fresh attempt
inline constexpr std::int64_t k_timed_out = 13888;

[[nodiscard]] exec::ErrorClass classify(std::int64_t code) noexcept;

[[nodiscard]] exec::ErrorClass classify_http(int status) noexcept;

// the target order already finished on its own
[[nodiscard]] bool is_settled_order_refusal(std::int64_t code) noexcept;

} // namespace raikiri::venues::deribit
