#include <raikiri/venues/bybit/errors.hpp>

namespace raikiri::venues::bybit {

exec::ErrorClass classify(std::int64_t ret_code) noexcept {
    switch (ret_code) {
    case k_ret_rate_limit:
    case k_ret_ip_banned:
        return exec::ErrorClass::RateLimited;

    case k_ret_invalid_api_key:
    case k_ret_sign_error:
    case k_ret_permission_denied:
        return exec::ErrorClass::AuthFailure;

    case k_ret_timestamp_error:
    case k_ret_server_timeout:
        return exec::ErrorClass::Retryable;

    default:
        return exec::ErrorClass::Terminal;
    }
}

bool is_benign_cancel_failure(std::int64_t ret_code) noexcept {
    return ret_code == k_ret_order_not_exists;
}

} // namespace raikiri::venues::bybit
