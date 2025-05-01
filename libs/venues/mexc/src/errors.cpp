#include <raikiri/venues/mexc/errors.hpp>

namespace raikiri::venues::mexc {

exec::ErrorClass classify(std::int64_t code) noexcept {
    switch (code) {
    case k_not_logged_in:
    case k_signature_invalid:
    case k_ip_not_whitelisted:
    case k_account_not_found:
        return exec::ErrorClass::AuthFailure;

    case k_rate_limited:
        return exec::ErrorClass::RateLimited;

    case k_internal_error:
    case k_system_busy:
        return exec::ErrorClass::Retryable;

    case k_request_expired:
        return exec::ErrorClass::Retryable;

    default:
        return exec::ErrorClass::Terminal;
    }
}

bool is_benign_cancel_failure(std::int64_t code) noexcept {
    return code == k_cancel_order_not_exist;
}

} // namespace raikiri::venues::mexc
