#include <raikiri/venues/deribit/errors.hpp>

namespace raikiri::venues::deribit {

exec::ErrorClass classify(std::int64_t code) noexcept {
    switch (code) {
    case k_invalid_credentials:
    case k_invalid_credentials_v1:
    case k_unauthorized:
    case k_forbidden:
        return exec::ErrorClass::AuthFailure;

    case k_too_many_requests:
    case k_too_many_concurrent_requests:
    case k_matching_engine_queue_full:
        return exec::ErrorClass::RateLimited;

    case k_retry:
    case k_settlement_in_progress:
    case k_temporarily_unavailable:
    case k_unavailable:
    case k_system_maintenance:
        return exec::ErrorClass::Retryable;

    case k_authorization_required:
        return exec::ErrorClass::Retryable;

    // A channel Deribit stopped serving.
    case k_subscribe_error_unsubscribed:
        return exec::ErrorClass::Retryable;

    default:
        return exec::ErrorClass::Terminal;
    }
}

} // namespace raikiri::venues::deribit
