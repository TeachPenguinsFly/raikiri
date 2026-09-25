#pragma once

// unrecognised codes default to terminal

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/error.hpp>

#include <cstdint>

namespace raikiri::venues::mexc {

using WireError = wire::Error;

// futures codes; zero means success
inline constexpr std::int64_t k_not_logged_in = 401; // no credentials
inline constexpr std::int64_t k_key_expired = 402;   // bogus api key
inline constexpr std::int64_t k_ip_not_whitelisted = 406;
inline constexpr std::int64_t k_internal_error = 500;
inline constexpr std::int64_t k_system_busy = 501;
inline constexpr std::int64_t k_unknown_source = 506;
inline constexpr std::int64_t k_rate_limited = 510;
inline constexpr std::int64_t k_endpoint_unreachable = 511;
// the clock-skew refusal; 603 is a duplicate request and must not be retried
inline constexpr std::int64_t k_request_expired = 513;
inline constexpr std::int64_t k_parameter_error = 600;
inline constexpr std::int64_t k_decode_error = 601;
inline constexpr std::int64_t k_signature_invalid = 602;
inline constexpr std::int64_t k_repeated_request = 603;
inline constexpr std::int64_t k_permission_read = 701;
inline constexpr std::int64_t k_permission_modify = 702;
inline constexpr std::int64_t k_permission_trade_read = 703;
inline constexpr std::int64_t k_permission_trade_modify = 704;
inline constexpr std::int64_t k_account_not_found = 1000;
inline constexpr std::int64_t k_contract_not_found = 1001;
inline constexpr std::int64_t k_contract_not_activated = 1002;
inline constexpr std::int64_t k_amount_error = 1004;
inline constexpr std::int64_t k_order_direction_invalid = 2001;
inline constexpr std::int64_t k_insufficient_balance = 2005;
inline constexpr std::int64_t k_leverage_error = 2006;
inline constexpr std::int64_t k_order_quantity_error = 2011;
inline constexpr std::int64_t k_cancel_over_limit = 2013;
inline constexpr std::int64_t k_client_order_id_too_long = 2030;
// per-order verdicts in a cancel response: 2040 unknown order, 2041 already finished
inline constexpr std::int64_t k_cancel_order_not_exist = 2040;
inline constexpr std::int64_t k_cancel_order_state = 2041;
inline constexpr std::int64_t k_trading_forbidden = 6001;
inline constexpr std::int64_t k_open_forbidden = 6002;

[[nodiscard]] exec::ErrorClass classify(std::int64_t code) noexcept;

// spot codes overlap the futures numbers with different meanings, hence two tables
inline constexpr std::int64_t k_spot_bad_request = 400;
inline constexpr std::int64_t k_spot_unauthorized = 401;
inline constexpr std::int64_t k_spot_forbidden = 403;
// a signed spot call missing timestamp and signature answers 404
inline constexpr std::int64_t k_spot_not_found = 404;
inline constexpr std::int64_t k_spot_rate_limited = 429;
inline constexpr std::int64_t k_spot_internal = 500;
inline constexpr std::int64_t k_spot_symbol_not_api = 10007;
inline constexpr std::int64_t k_spot_api_key_invalid = 10072;
inline constexpr std::int64_t k_spot_insufficient_balance = 10101;
inline constexpr std::int64_t k_spot_trading_disabled = 30016;
inline constexpr std::int64_t k_spot_order_not_exist = 30025;
inline constexpr std::int64_t k_spot_key_missing = 700001;
inline constexpr std::int64_t k_spot_signature_invalid = 700002;
inline constexpr std::int64_t k_spot_request_expired = 700003;
inline constexpr std::int64_t k_spot_order_target_missing = 700004;
inline constexpr std::int64_t k_spot_recv_window_too_large = 700005;
inline constexpr std::int64_t k_spot_ip_not_whitelisted = 700006;
inline constexpr std::int64_t k_spot_no_permission = 700007;

[[nodiscard]] exec::ErrorClass classify_spot(std::int64_t code) noexcept;

[[nodiscard]] bool is_benign_spot_cancel_failure(std::int64_t code) noexcept;

// the order already filled or was cancelled; reconciliation settles it
[[nodiscard]] bool is_benign_cancel_failure(std::int64_t code) noexcept;

} // namespace raikiri::venues::mexc
