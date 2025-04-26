#include <raikiri/venues/bitget/errors.hpp>

#include <charconv>

namespace raikiri::venues::bitget {

exec::ErrorClass classify(std::string_view code) noexcept {
    if (code == k_access_key_empty || code == k_secret_key_empty || code == k_signature_empty ||
        code == k_invalid_access_key || code == k_signature_error) {
        return exec::ErrorClass::AuthFailure;
    }
    if (code == k_too_many_requests) {
        return exec::ErrorClass::RateLimited;
    }
    // Clock skew, not bad credentials: the same payload signed against a
    // corrected offset succeeds, so retrying after a resync is right. Filing
    // this under AuthFailure would halt a venue whose keys are fine.
    if (code == k_timestamp_expired) {
        return exec::ErrorClass::Retryable;
    }
    if (code == k_system_error) {
        return exec::ErrorClass::Retryable;
    }
    return exec::ErrorClass::Terminal;
}

std::int64_t code_number(std::string_view code) noexcept {
    std::int64_t out = 0;
    const auto* const end = code.data() + code.size();
    const auto [stop, ec] = std::from_chars(code.data(), end, out);
    return (ec == std::errc{} && stop == end) ? out : 0;
}

} // namespace raikiri::venues::bitget
