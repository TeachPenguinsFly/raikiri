#include <raikiri/venues/kraken/errors.hpp>

namespace raikiri::venues::kraken {

namespace {

// the name up to the first ':'
std::string_view name_of(std::string_view error) noexcept {
    if (const auto colon = error.find(':'); colon != std::string_view::npos) {
        error.remove_suffix(error.size() - colon);
    }
    return error;
}

} // namespace

exec::ErrorClass classify(std::string_view error) noexcept {
    const auto name = name_of(error);

    // unauthenticated reads answer this under http 200
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
    // routing-layer failures carry status, not error
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

bool is_send_accepted(std::string_view status) noexcept {
    // an ioc or market order may fill before the response; that is not a refusal
    return status == "placed" || status == "filled" || status == "partiallyFilled";
}

bool is_cancel_accepted(std::string_view status) noexcept {
    return status == "cancelled" || status == "filled" || status == "notFound";
}

bool is_edit_accepted(std::string_view status) noexcept {
    return status == "edited";
}

} // namespace raikiri::venues::kraken
