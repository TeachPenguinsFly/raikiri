#include <raikiri/venues/mexc/errors.hpp>

namespace raikiri::venues::mexc {

exec::ErrorClass classify(std::int64_t code) noexcept {
    switch (code) {
    case k_not_logged_in:
    case k_key_expired:
    case k_signature_invalid:
    case k_ip_not_whitelisted:
    case k_unknown_source:
    case k_account_not_found:
    case k_permission_read:
    case k_permission_modify:
    case k_permission_trade_read:
    case k_permission_trade_modify:
        return exec::ErrorClass::AuthFailure;

    case k_rate_limited:
        return exec::ErrorClass::RateLimited;

    case k_internal_error:
    case k_system_busy:
    case k_endpoint_unreachable:
        return exec::ErrorClass::Retryable;

    // The same payload signed against a corrected offset succeeds. A REPEATED
    // request (603) is deliberately not here: retrying one places the order
    // twice.
    case k_request_expired:
        return exec::ErrorClass::Retryable;

    default:
        return exec::ErrorClass::Terminal;
    }
}

exec::ErrorClass classify_spot(std::int64_t code) noexcept {
    switch (code) {
    case k_spot_bad_request:
    case k_spot_unauthorized:
    case k_spot_forbidden:
    case k_spot_api_key_invalid:
    case k_spot_key_missing:
    case k_spot_signature_invalid:
    case k_spot_ip_not_whitelisted:
    case k_spot_no_permission:
        return exec::ErrorClass::AuthFailure;

    case k_spot_rate_limited:
        return exec::ErrorClass::RateLimited;

    case k_spot_internal:
        return exec::ErrorClass::Retryable;

    // A stale timestamp is retryable because the next attempt carries a fresh
    // one; a recvWindow we chose is not, and belongs with the config bugs.
    case k_spot_request_expired:
        return exec::ErrorClass::Retryable;

    default:
        return exec::ErrorClass::Terminal;
    }
}

bool is_benign_spot_cancel_failure(std::int64_t code) noexcept {
    return code == k_spot_order_not_exist;
}

bool is_benign_cancel_failure(std::int64_t code) noexcept {
    return code == k_cancel_order_not_exist || code == k_cancel_order_state;
}

} // namespace raikiri::venues::mexc
