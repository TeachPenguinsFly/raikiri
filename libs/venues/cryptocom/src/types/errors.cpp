#include <raikiri/venues/cryptocom/errors.hpp>

namespace raikiri::venues::cryptocom {

exec::ErrorClass classify(std::int64_t code) noexcept {
    switch (code) {
    case k_unauthorized:
    case k_ip_illegal:
        return exec::ErrorClass::AuthFailure;

    case k_too_many_requests:
        return exec::ErrorClass::RateLimited;

    case k_request_timeout:
    case k_internal:
    case k_invalid_nonce:
        return exec::ErrorClass::Retryable;

    default:
        return exec::ErrorClass::Terminal;
    }
}

} // namespace raikiri::venues::cryptocom
