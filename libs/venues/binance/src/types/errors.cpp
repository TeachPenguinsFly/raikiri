#include <raikiri/venues/binance/errors.hpp>

namespace raikiri::venues::binance {

exec::ErrorClass classify(std::int64_t code) noexcept {
    switch (code) {
    case k_unauthorized:
    case k_invalid_signature:
    case k_bad_api_key_format:
    case k_rejected_key_or_ip:
        return exec::ErrorClass::AuthFailure;

    case k_too_many_requests:
    case k_too_many_orders:
        return exec::ErrorClass::RateLimited;

    case k_unknown:
    case k_disconnected:
    case k_unexpected_response:
    case k_timeout:
    case k_server_busy:
    case k_service_shutting_down:
        return exec::ErrorClass::Retryable;

    case k_invalid_timestamp:
        return exec::ErrorClass::Retryable;

    // credentials are valid; the stream key is not
    case k_invalid_listen_key:
        return exec::ErrorClass::Retryable;

    default:
        return exec::ErrorClass::Terminal;
    }
}

bool is_benign_cancel_failure(std::int64_t code) noexcept {
    return code == k_cancel_rejected || code == k_no_such_order;
}

bool is_listen_key_failure(std::int64_t code) noexcept {
    return code == k_invalid_listen_key;
}

} // namespace raikiri::venues::binance
