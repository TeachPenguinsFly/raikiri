#pragma once

// unknown codes default to Terminal: retrying an unknown refusal risks an ip ban

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/error.hpp>

#include <cstdint>

namespace raikiri::venues::binance {

using WireError = wire::Error;

// Binance codes are negative; zero means no error.
inline constexpr std::int64_t k_ok = 0;
inline constexpr std::int64_t k_unknown = -1000;
inline constexpr std::int64_t k_disconnected = -1001;
inline constexpr std::int64_t k_unauthorized = -1002;
inline constexpr std::int64_t k_too_many_requests = -1003;
inline constexpr std::int64_t k_unexpected_response = -1006;
inline constexpr std::int64_t k_timeout = -1007;
inline constexpr std::int64_t k_server_busy = -1008;
inline constexpr std::int64_t k_too_many_orders = -1015;
inline constexpr std::int64_t k_service_shutting_down = -1016;
inline constexpr std::int64_t k_invalid_timestamp = -1021; // outside recvWindow
inline constexpr std::int64_t k_invalid_signature = -1022;
inline constexpr std::int64_t k_illegal_chars = -1100;
inline constexpr std::int64_t k_mandatory_param_empty = -1102;
inline constexpr std::int64_t k_bad_precision = -1111;
inline constexpr std::int64_t k_invalid_side = -1117;
inline constexpr std::int64_t k_bad_symbol = -1121;
inline constexpr std::int64_t k_invalid_listen_key = -1125;
inline constexpr std::int64_t k_duplicate_client_order_id = -4015;
inline constexpr std::int64_t k_new_order_rejected = -2010;
inline constexpr std::int64_t k_cancel_rejected = -2011;
inline constexpr std::int64_t k_no_such_order = -2013;
inline constexpr std::int64_t k_bad_api_key_format = -2014;
inline constexpr std::int64_t k_rejected_key_or_ip = -2015;
inline constexpr std::int64_t k_balance_insufficient = -2018;
inline constexpr std::int64_t k_margin_insufficient = -2019;
inline constexpr std::int64_t k_would_trigger_immediately = -2021;
inline constexpr std::int64_t k_reduce_only_rejected = -2022;
inline constexpr std::int64_t k_min_notional = -4164;
inline constexpr std::int64_t k_post_only_would_take = -5021; // GTX crossed the book

[[nodiscard]] exec::ErrorClass classify(std::int64_t code) noexcept;

// an unknown-order cancel usually means already filled or cancelled; reconciliation settles it
[[nodiscard]] bool is_benign_cancel_failure(std::int64_t code) noexcept;

// the listen key expired; recreate it instead of halting on auth
[[nodiscard]] bool is_listen_key_failure(std::int64_t code) noexcept;

} // namespace raikiri::venues::binance
