#pragma once

// HTX error classification.
//
// The generic gateway retries Retryable, backs off RateLimited, halts on
// AuthFailure and synthesizes a terminal Rejected for Terminal — so a code in
// the wrong bucket is a trading bug, not a logging detail. Unrecognised codes
// default to Terminal: an unknown refusal repeated at full speed is how an
// account earns an IP ban, while a spuriously terminal order is recoverable
// by reconciliation.
//
// NOT YET KNOWN, and load-bearing when signing lands: HTX checks the access
// key before the timestamp, so the window refusal cannot be provoked without
// credentials. Whatever its code is, it belongs in Retryable — clock skew is
// fixed by resyncing, not by halting a venue whose keys are fine.

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/json.hpp>

#include <cstdint>
#include <string_view>

namespace raikiri::venues::htx {

// Every parser in this venue reports failure with this. It lives beside the
// code vocabulary because both answer one question: what did the venue say
// went wrong.
using WireError = wire::Error;

// The whole signature/key family: a missing SignatureMethod and a bogus
// AccessKeyId both answer 403, differing only in message.
inline constexpr std::int64_t k_signature_rejected = 403;

inline constexpr std::int64_t k_contract_not_found = 1014;
inline constexpr std::int64_t k_parameter_empty = 1066;
inline constexpr std::int64_t k_parameter_illegal = 1067;

// The swap-specific spelling of "no such contract": a different endpoint
// family answering the same mistake.
inline constexpr std::int64_t k_swap_not_found = 1332;

inline constexpr std::int64_t k_notify_contract_not_found = 2011;

// The market socket's refusal for a topic it cannot parse, and
// linear-swap-ex's for a bad query parameter. Words, not numbers.
inline constexpr std::string_view k_ws_bad_request = "bad-request";
inline constexpr std::string_view k_invalid_parameter = "invalid-parameter";

// Order entry, linear swap. Numbers here, words on spot: one venue, two
// vocabularies, and classify() reads whichever arrived. Never match the futures
// err_msg text — the wire drops apostrophes the docs carry ("doesnt") and
// spells 1071 "Repeated withdraw." where the table says "Repeated
// cancellation." — the integer is the only stable half.
inline constexpr std::int64_t k_swap_query_error = 1002;
inline constexpr std::int64_t k_swap_redis_error = 1003;
inline constexpr std::int64_t k_swap_system_busy = 1004;
inline constexpr std::int64_t k_swap_contract_missing = 1014;

// Not a bad request: HTX's own FAQ names 1032 as the derivative rate limit,
// even though the static code table gives it the same words as 1052.
inline constexpr std::int64_t k_swap_rate_limited = 1032;

inline constexpr std::int64_t k_swap_service_abnormal = 1046;
inline constexpr std::int64_t k_swap_insufficient_margin = 1047;
inline constexpr std::int64_t k_swap_nothing_to_cancel = 1051;
inline constexpr std::int64_t k_swap_order_missing = 1061;
inline constexpr std::int64_t k_swap_cancel_in_flight = 1062;
inline constexpr std::int64_t k_swap_already_executed = 1063;

// The one that would fire on every order if the ULID travelled: the swap types
// client_order_id as an int64 and refuses anything else.
inline constexpr std::int64_t k_swap_client_id_not_integer = 1065;

inline constexpr std::int64_t k_swap_already_cancelled = 1071;

// Placing and cancelling more than 3000 limit/post_only/ioc/fok orders in ten
// minutes at a >99% cancel ratio bans those four types for five minutes. It is
// a throttle with a `recovery-time` header, not a refusal of the order.
inline constexpr std::int64_t k_swap_cancel_ratio_ban = 1084;

inline constexpr std::int64_t k_swap_service_unavailable = 1108;

// The detail family behind a 403. In practice 403 arrives first because the
// access-key check short-circuits, but a key that passes it and fails here
// still has to halt the venue rather than retry.
inline constexpr std::int64_t k_swap_auth_detail_first = 12'001;
inline constexpr std::int64_t k_swap_auth_detail_last = 12'009;

// Spot names its refusals in words. Only the two the venue actually answers
// with are listed: the signature family collapses into one code whose reason
// lives in err-msg, and "login-required" is what a missing key or signature
// gets (verified live, unauthenticated).
inline constexpr std::string_view k_spot_login_required = "login-required";
inline constexpr std::string_view k_spot_signature_invalid = "api-signature-not-valid";

// Cancelling something that already finished. Neither surface is an error we
// can act on: the order is gone and reconciliation settles what it became.
inline constexpr std::string_view k_spot_order_state = "order-orderstate-error";
inline constexpr std::string_view k_spot_record_invalid = "base-record-invalid";
inline constexpr std::string_view k_spot_not_found = "not-found";
inline constexpr std::string_view k_spot_base_not_found = "base-not-found";

// Cancel-by-client-order-id answers an integer STATE, and 0 means the id was
// never found. It has no err-code of its own, so the adapter mints this token
// to keep one vocabulary reaching classify().
inline constexpr std::string_view k_client_id_not_found = "client-order-id-not-found";

// HTX answers a throttled caller with HTTP 429, and spot echoes the number into
// err-code. Spot documents no REST rate-limit code beyond that.
inline constexpr std::int64_t k_too_many_requests = 429;

[[nodiscard]] exec::ErrorClass classify(std::string_view code) noexcept;

// A cancel that failed because the order is no longer there. The order is in
// exactly the state it would be in had the cancel worked, so turning this into
// a CancelRejected would tell the OMS the order is still live.
[[nodiscard]] bool is_benign_cancel_failure(std::string_view code) noexcept;

// The signals that need no code table: HTX answers a throttled caller with
// HTTP 429 and an unrouted path with a Spring 404 carrying no envelope.
[[nodiscard]] exec::ErrorClass classify_http(int status) noexcept;

} // namespace raikiri::venues::htx
