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

    // Clock skew beyond recv_window looks like a signing failure but is
    // fixed by resyncing time, not by rotating keys — retrying gives the
    // clock-sync poller a chance to correct the offset first.
    case k_ret_timestamp_error:
    case k_ret_server_timeout:
        return exec::ErrorClass::Retryable;

    default:
        // Every named business refusal below is terminal, and so is anything
        // unrecognised: repeating an unknown refusal at full speed is how an
        // account earns a ban.
        return exec::ErrorClass::Terminal;
    }
}

bool is_benign_cancel_failure(std::int64_t ret_code) noexcept {
    // "Order does not exist" on a cancel almost always means the order
    // filled or was already cancelled between our decision and the request.
    // Reconciliation settles which; raising an alarm here would train
    // operators to ignore cancel rejections that do matter.
    return ret_code == k_ret_order_not_exists;
}

} // namespace raikiri::venues::bybit
