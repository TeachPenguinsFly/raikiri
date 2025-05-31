#pragma once

// Bitget error-code classification. A code in the wrong bucket is a trading
// bug; unknown codes default to Terminal, because a refusal repeated at full
// speed is how an account earns an IP ban.
//
// REST codes are STRINGS and success is "00000", so classification compares
// text: converting first collapses "00000", "0" and an unreadable field onto
// the same integer, and only one of those is success. Websocket codes are a
// different space AND a different type — unquoted numbers — and are recorded
// rather than classified, since a refused subscription is terminal for the
// channel it names whatever the code says.

#include <raikiri/exec/error.hpp>

#include <cstdint>
#include <string_view>

namespace raikiri::venues::bitget {

// The first four VERIFIED live on 2026-08-11; the rest from the v2 reference.
inline constexpr std::string_view k_invalid_access_key = "40006";
inline constexpr std::string_view k_parameter_not_exist = "40034";
inline constexpr std::string_view k_parameter_check_failed = "400172";
inline constexpr std::string_view k_url_not_found = "40404";
inline constexpr std::string_view k_access_key_empty = "40001";
inline constexpr std::string_view k_secret_key_empty = "40002";
inline constexpr std::string_view k_signature_empty = "40003";
inline constexpr std::string_view k_timestamp_expired = "40008";
inline constexpr std::string_view k_signature_error = "40009";
inline constexpr std::string_view k_parameter_verification = "40017";
inline constexpr std::string_view k_system_error = "25000";
inline constexpr std::string_view k_too_many_requests = "429";

// Trade-path codes. Bitget publishes one 827-row table under both the spot
// and the futures doc paths; they are byte-identical, so one map serves both.
inline constexpr std::string_view k_content_type_invalid = "40007";
inline constexpr std::string_view k_passphrase_empty = "40011";
inline constexpr std::string_view k_apikey_password_incorrect = "40012";
inline constexpr std::string_view k_user_status_abnormal = "40013";
inline constexpr std::string_view k_permission_missing = "40014";
inline constexpr std::string_view k_invalid_ip = "40018";
inline constexpr std::string_view k_no_permission = "40025";
inline constexpr std::string_view k_passphrase_error = "40036";
// VERIFIED live 2026-08-25: a syntactically valid key that does not exist.
inline constexpr std::string_view k_apikey_absent = "40037";
inline constexpr std::string_view k_ip_not_whitelisted = "40038";
inline constexpr std::string_view k_apikey_not_found = "40041";

// 40008 above is the 30-second window expiring; this one is the ME window.
inline constexpr std::string_view k_recv_window_expired = "40078";

inline constexpr std::string_view k_request_timed_out = "40010";
inline constexpr std::string_view k_system_busy = "40015";
inline constexpr std::string_view k_unknown_error = "45001";
inline constexpr std::string_view k_cancel_order_error = "45024";
inline constexpr std::string_view k_retry_request = "43059";
inline constexpr std::string_view k_trade_suspended = "45043";

inline constexpr std::string_view k_cancel_too_frequent = "45129";
inline constexpr std::string_view k_open_too_frequent = "40918";

// A cancel that failed because the order was already gone. Bitget spells the
// same outcome six ways across the two APIs.
inline constexpr std::string_view k_order_not_exist = "43001";
inline constexpr std::string_view k_order_not_exist_alt = "45057";
inline constexpr std::string_view k_order_not_exist_mix = "31007";
inline constexpr std::string_view k_order_finalized = "45031";
inline constexpr std::string_view k_no_order_to_cancel = "22001";
inline constexpr std::string_view k_no_order_to_cancel_alt = "43004";

// VERIFIED live against wss://ws.bitget.com/v2/ws/public.
inline constexpr std::int64_t k_ws_unknown_subscription = 30001;
inline constexpr std::int64_t k_ws_unrecognized_request = 30002;
inline constexpr std::int64_t k_ws_param_error = 30016;

// Private-handshake codes. The first two are VERIFIED live against
// wss://ws.bitget.com/v2/ws/private; the rest are the documented siblings.
// Every one of them is fatal for the connection — Bitget closes the socket
// when a login fails, so a retry loop here is a reconnect loop.
inline constexpr std::int64_t k_ws_not_logged_in = 30004;
inline constexpr std::int64_t k_ws_invalid_key = 30011;
inline constexpr std::int64_t k_ws_login_failed = 30005;
inline constexpr std::int64_t k_ws_invalid_passphrase = 30012;
inline constexpr std::int64_t k_ws_invalid_timestamp = 30013;
inline constexpr std::int64_t k_ws_timestamp_expired = 30014;
inline constexpr std::int64_t k_ws_invalid_sign = 30015;

[[nodiscard]] bool is_login_failure(std::int64_t ws_code) noexcept;

// A cancel refused because the order is already gone is not a cancel failure.
// Reporting CancelRejected for it puts a terminal order back into doubt at
// exactly the moment the local view was right.
[[nodiscard]] bool is_benign_cancel_failure(std::string_view code) noexcept;

[[nodiscard]] exec::ErrorClass classify(std::string_view code) noexcept;

// Numeric form for exec::VenueError's int64 field. A non-numeric code yields
// 0, which collides with "00000" — classification never reads this value.
[[nodiscard]] std::int64_t code_number(std::string_view code) noexcept;

} // namespace raikiri::venues::bitget
