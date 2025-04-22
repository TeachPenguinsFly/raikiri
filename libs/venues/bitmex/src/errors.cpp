#include <raikiri/venues/bitmex/errors.hpp>

namespace raikiri::venues::bitmex {

exec::ErrorClass classify(std::int64_t status, std::string_view name) noexcept {
    // The one name worth trusting over the status: BitMEX documents answering a
    // breached budget with it, and a rate limit filed as Terminal would give up
    // on a venue that only asked us to wait.
    if (name == "RateLimitError") {
        return exec::ErrorClass::RateLimited;
    }
    switch (status) {
    case k_too_many_requests:
        return exec::ErrorClass::RateLimited;

    case k_unauthorized:
    case k_forbidden:
        return exec::ErrorClass::AuthFailure;

    case k_request_timeout:
    case k_internal_error:
    case k_bad_gateway:
    case k_service_unavailable:
    case k_gateway_timeout:
        return exec::ErrorClass::Retryable;

    default:
        // 400 and 404 land here, and both are terminal: a symbol BitMEX does
        // not know will still not exist on the next attempt.
        return exec::ErrorClass::Terminal;
    }
}

} // namespace raikiri::venues::bitmex
