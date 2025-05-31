#include <raikiri/venues/bitget/errors.hpp>

#include <charconv>

namespace raikiri::venues::bitget {

exec::ErrorClass classify(std::string_view code) noexcept {
    if (code == k_access_key_empty || code == k_secret_key_empty || code == k_signature_empty ||
        code == k_invalid_access_key || code == k_signature_error || code == k_content_type_invalid || code == k_passphrase_empty ||
        code == k_apikey_password_incorrect || code == k_user_status_abnormal ||
        code == k_permission_missing || code == k_invalid_ip || code == k_no_permission ||
        code == k_passphrase_error || code == k_apikey_absent || code == k_ip_not_whitelisted ||
        code == k_apikey_not_found) {
        return exec::ErrorClass::AuthFailure;
    }
    if (code == k_too_many_requests || code == k_cancel_too_frequent ||
        code == k_open_too_frequent) {
        return exec::ErrorClass::RateLimited;
    }
    // Clock skew alone: a resync fixes it, a retry without one repeats it.
    // 40005 is NOT here — a malformed timestamp is our bug, not the venue's.
    if (code == k_timestamp_expired || code == k_recv_window_expired) {
        return exec::ErrorClass::Retryable;
    }
    // Transient venue-side conditions. 45023 "Error creating order" is
    // deliberately absent: a create whose outcome is unknown must never be
    // repeated, and a duplicate fill costs more than a missed order.
    if (code == k_system_error || code == k_request_timed_out || code == k_system_busy ||
        code == k_unknown_error || code == k_cancel_order_error || code == k_retry_request ||
        code == k_trade_suspended) {
        return exec::ErrorClass::Retryable;
    }
    return exec::ErrorClass::Terminal;
}

bool is_login_failure(std::int64_t ws_code) noexcept {
    return ws_code == k_ws_not_logged_in || ws_code == k_ws_login_failed ||
           ws_code == k_ws_invalid_key || ws_code == k_ws_invalid_passphrase ||
           ws_code == k_ws_invalid_timestamp || ws_code == k_ws_timestamp_expired ||
           ws_code == k_ws_invalid_sign;
}

bool is_benign_cancel_failure(std::string_view code) noexcept {
    return code == k_order_not_exist || code == k_order_not_exist_alt ||
           code == k_order_not_exist_mix || code == k_order_finalized ||
           code == k_no_order_to_cancel || code == k_no_order_to_cancel_alt;
}

std::int64_t code_number(std::string_view code) noexcept {
    std::int64_t out = 0;
    const auto* const end = code.data() + code.size();
    const auto [stop, ec] = std::from_chars(code.data(), end, out);
    return (ec == std::errc{} && stop == end) ? out : 0;
}

} // namespace raikiri::venues::bitget
