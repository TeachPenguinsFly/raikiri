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

    // The venue asked us to come back, or is mid-settlement, or is down for
    // maintenance. All of these end on their own.
    case k_retry:
    case k_settlement_in_progress:
    case k_temporarily_unavailable:
    case k_unavailable:
    case k_system_maintenance:
        return exec::ErrorClass::Retryable;

    // The SESSION lapsed, not the credentials — authenticating again is the
    // fix. Filing this under AuthFailure would halt a venue whose keys are
    // fine, which is the same mistake as reading a clock-skew refusal as a
    // bad signature.
    case k_authorization_required:
        return exec::ErrorClass::Retryable;

    // A channel Deribit stopped serving. Resubscribing is exactly the
    // reconnect path's job, so this must not be terminal.
    case k_subscribe_error_unsubscribed:
        return exec::ErrorClass::Retryable;

    default:
        return exec::ErrorClass::Terminal;
    }
}

} // namespace raikiri::venues::deribit
