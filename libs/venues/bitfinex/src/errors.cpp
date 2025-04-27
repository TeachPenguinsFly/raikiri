#include <raikiri/venues/bitfinex/errors.hpp>

namespace raikiri::venues::bitfinex {

exec::ErrorClass classify(std::int64_t code) noexcept {
    switch (code) {
    case k_auth_failed:
    case k_auth_payload:
    case k_auth_signature:
    case k_auth_encryption:
    case k_auth_fail:
        return exec::ErrorClass::AuthFailure;

    case k_rate_limit:
        return exec::ErrorClass::RateLimited;

    // The nonce ran backwards against what the venue has already seen — clock
    // skew or a restarted counter, not bad credentials. The same key signs
    // successfully on the next attempt, so filing this under AuthFailure would
    // halt a venue whose keys are fine.
    case k_auth_nonce:
        return exec::ErrorClass::Retryable;

    // Both maintenance notices and the stop notice reissue channel ids on the
    // far side; the connection recovers, so the venue is not down for good.
    case k_server_stopping:
    case k_maintenance_start:
    case k_maintenance_end:
        return exec::ErrorClass::Retryable;

    // Too many connections opening at once. Spacing them out is the fix.
    case k_concurrency_limit:
        return exec::ErrorClass::Retryable;

    default:
        return exec::ErrorClass::Terminal;
    }
}

} // namespace raikiri::venues::bitfinex
