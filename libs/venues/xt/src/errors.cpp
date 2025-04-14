#include <raikiri/venues/xt/errors.hpp>

#include <charconv>

namespace raikiri::venues::xt {

std::int64_t numeric_code(std::string_view code) noexcept {
    if (code.empty()) {
        return 0;
    }
    std::int64_t out = 0;
    const auto* const end = code.data() + code.size();
    const auto [stop, ec] = std::from_chars(code.data(), end, out);
    return (ec == std::errc{} && stop == end) ? out : 0;
}

exec::ErrorClass classify(std::string_view code) noexcept {
    if (code == k_invalid_symbol || code == k_invalid_param) {
        return exec::ErrorClass::Terminal;
    }

    // The numeric family is HTTP statuses verbatim: XT answers an unsigned
    // private call with "400", so the transport's own vocabulary reaches the
    // envelope and the standard reading of the rest of that range applies.
    switch (numeric_code(code)) {
    case 401:
    case 403:
        return exec::ErrorClass::AuthFailure;
    case 429:
        return exec::ErrorClass::RateLimited;
    // A timeout and a 5xx are the same decision: the request never reached a
    // verdict, so repeating it is the only way to learn one.
    case 408:
    case 500:
    case 502:
    case 503:
    case 504:
        return exec::ErrorClass::Retryable;
    default:
        // Includes "400", which XT returns both for a missing appKey and for a
        // malformed request. Conflating those two under AuthFailure would halt
        // a venue whose keys are fine, so the ambiguous status stays Terminal.
        return exec::ErrorClass::Terminal;
    }
}

} // namespace raikiri::venues::xt
