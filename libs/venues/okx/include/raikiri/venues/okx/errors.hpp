#pragma once

// OKX error-code classification.
//
// The generic gateway retries Retryable, backs off RateLimited, halts on
// AuthFailure and synthesizes a terminal Rejected for Terminal — so a code in
// the wrong bucket is a trading bug, not a logging detail.
//
// Unrecognised codes default to Terminal, not Retryable: an unknown refusal
// repeated at full speed is how an account earns an IP ban, while a
// spuriously terminal order is recoverable by reconciliation.

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/error.hpp>

#include <cstdint>

namespace raikiri::venues::okx {

// Every parser in this venue reports failure with this. It lives beside the
// code vocabulary because both answer one question: what did the venue say
// went wrong.
using WireError = wire::Error;

// OKX codes are non-negative; zero means success.
inline constexpr std::int64_t k_operation_failed = 1;
inline constexpr std::int64_t k_bulk_partial = 2;
inline constexpr std::int64_t k_service_unavailable = 50001;
inline constexpr std::int64_t k_endpoint_timeout = 50004;
inline constexpr std::int64_t k_api_offline = 50005;
inline constexpr std::int64_t k_too_many_requests = 50011;
inline constexpr std::int64_t k_system_busy = 50013;
inline constexpr std::int64_t k_system_error = 50026;
inline constexpr std::int64_t k_key_wrong_environment = 50101;
inline constexpr std::int64_t k_timestamp_expired = 50102; // outside the 30 s window
inline constexpr std::int64_t k_api_key_empty = 50103;
inline constexpr std::int64_t k_passphrase_empty = 50104;
inline constexpr std::int64_t k_passphrase_incorrect = 50105;
inline constexpr std::int64_t k_signature_empty = 50106;
inline constexpr std::int64_t k_timestamp_empty = 50107;
inline constexpr std::int64_t k_ip_not_whitelisted = 50110;
inline constexpr std::int64_t k_invalid_api_key = 50111;
inline constexpr std::int64_t k_invalid_timestamp = 50112;
inline constexpr std::int64_t k_invalid_signature = 50113;
inline constexpr std::int64_t k_duplicate_param = 50071; // "clOrdId already exists"
inline constexpr std::int64_t k_parameter_error = 51000;
inline constexpr std::int64_t k_instrument_not_found = 51001; // VERIFIED live
inline constexpr std::int64_t k_insufficient_balance = 51008;
inline constexpr std::int64_t k_order_price_out_of_range = 51006;
inline constexpr std::int64_t k_post_only_would_take = 51121;
inline constexpr std::int64_t k_cancel_order_not_exist = 51400;
inline constexpr std::int64_t k_cancel_already_canceled = 51401;
inline constexpr std::int64_t k_cancel_already_completed = 51402;
inline constexpr std::int64_t k_amend_already_settled = 51503;
inline constexpr std::int64_t k_order_not_found = 51603;
inline constexpr std::int64_t k_ws_timestamp_expired = 60006;
inline constexpr std::int64_t k_ws_invalid_signature = 60007;
inline constexpr std::int64_t k_ws_login_failed = 60009;
inline constexpr std::int64_t k_ws_login_required = 60011;
inline constexpr std::int64_t k_ws_invalid_request = 60012;
inline constexpr std::int64_t k_ws_too_frequent = 60014;
inline constexpr std::int64_t k_ws_wrong_channel = 60018; // VERIFIED live
inline constexpr std::int64_t k_internal_system_error = 63999;
inline constexpr std::int64_t k_tier_too_low_for_channel = 64003; // e.g. the fills channel

[[nodiscard]] exec::ErrorClass classify(std::int64_t code) noexcept;

// A cancel for an order OKX has already finished with usually means the order
// filled or was already cancelled; reconciliation settles the truth. Callers
// use this to avoid an alarming CancelRejected for a benign race.
[[nodiscard]] bool is_benign_cancel_failure(std::int64_t code) noexcept;

// Same reasoning for an amend: OKX refuses to modify an order that has
// already settled, and the private stream has by then already said so. A
// ModifyRejected for one would be a transition the order can no longer make.
[[nodiscard]] bool is_benign_amend_failure(std::int64_t code) noexcept;

} // namespace raikiri::venues::okx
