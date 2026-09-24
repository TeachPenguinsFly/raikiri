#include <raikiri/venues/okx/errors.hpp>

namespace raikiri::venues::okx {

exec::ErrorClass classify(std::int64_t code) noexcept {
    switch (code) {
    case k_key_wrong_environment:
    case k_api_key_empty:
    case k_passphrase_empty:
    case k_passphrase_incorrect:
    case k_signature_empty:
    case k_timestamp_empty:
    case k_ip_not_whitelisted:
    case k_invalid_api_key:
    case k_invalid_signature:
    case k_ws_invalid_signature:
    case k_ws_login_failed:
        return exec::ErrorClass::AuthFailure;

    case k_too_many_requests:
    case k_ws_too_frequent:
        return exec::ErrorClass::RateLimited;

    case k_service_unavailable:
    case k_endpoint_timeout:
    case k_api_offline:
    case k_system_busy:
    case k_system_error:
    case k_internal_system_error:
        return exec::ErrorClass::Retryable;

    case k_timestamp_expired:
    case k_invalid_timestamp:
    case k_ws_timestamp_expired:
        return exec::ErrorClass::Retryable;

    // the session lapsed; logging in again fixes it
    case k_ws_login_required:
        return exec::ErrorClass::Retryable;

    default:
        return exec::ErrorClass::Terminal;
    }
}

bool is_benign_cancel_failure(std::int64_t code) noexcept {
    return code == k_cancel_order_not_exist || code == k_cancel_already_canceled ||
           code == k_cancel_already_completed || code == k_order_not_found;
}

bool is_benign_amend_failure(std::int64_t code) noexcept {
    return code == k_amend_already_settled || is_benign_cancel_failure(code);
}

} // namespace raikiri::venues::okx
