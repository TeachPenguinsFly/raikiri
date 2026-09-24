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

    case k_auth_nonce:
        return exec::ErrorClass::Retryable;

    case k_server_stopping:
    case k_maintenance_start:
    case k_maintenance_end:
        return exec::ErrorClass::Retryable;

    // too many connections opening at once
    case k_concurrency_limit:
        return exec::ErrorClass::Retryable;

    default:
        return exec::ErrorClass::Terminal;
    }
}

} // namespace raikiri::venues::bitfinex
