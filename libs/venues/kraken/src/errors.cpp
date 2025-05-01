#include <raikiri/venues/kraken/errors.hpp>

namespace raikiri::venues::kraken {

namespace {

// The name up to the first ':'. "invalidArgument: symbol" and
// "invalidArgument: orderType" are one condition reported against two fields.
std::string_view name_of(std::string_view error) noexcept {
    if (const auto colon = error.find(':'); colon != std::string_view::npos) {
        error.remove_suffix(error.size() - colon);
    }
    return error;
}

} // namespace

exec::ErrorClass classify(std::string_view error) noexcept {
    const auto name = name_of(error);

    // VERIFIED: unauthenticated GET /accounts answers this under HTTP 200.
    if (name == "authenticationError" || name == "invalidAccount" || name == "accountInactive" ||
        name == "insufficientPermissions") {
        return exec::ErrorClass::AuthFailure;
    }
    if (name == "nonceBelowThreshold" || name == "nonceDuplicate") {
        return exec::ErrorClass::Retryable;
    }
    if (name == "apiLimitExceeded" || name == "tooManyRequests") {
        return exec::ErrorClass::RateLimited;
    }
    if (name == "unavailable" || name == "marketUnavailable" || name == "serviceUnavailable" ||
        name == "temporarilyUnavailable") {
        return exec::ErrorClass::Retryable;
    }
    // Routing-layer failures: a `status`, not an `error`, and no verdict.
    if (name == "INTERNAL_SERVER_ERROR" || name == "SERVICE_UNAVAILABLE" ||
        name == "GATEWAY_TIMEOUT") {
        return exec::ErrorClass::Retryable;
    }
    return exec::ErrorClass::Terminal;
}

bool is_benign_cancel_failure(std::string_view error) noexcept {
    const auto name = name_of(error);
    return name == "orderForEditNotFound" || name == "orderNotFound" ||
           name == "cancelOrderNotFound";
}

} // namespace raikiri::venues::kraken
