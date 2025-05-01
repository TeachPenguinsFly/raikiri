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

    switch (numeric_code(code)) {
    case 401:
    case 403:
        return exec::ErrorClass::AuthFailure;
    case 429:
        return exec::ErrorClass::RateLimited;
    case 408:
    case 500:
    case 502:
    case 503:
    case 504:
        return exec::ErrorClass::Retryable;
    default:
        return exec::ErrorClass::Terminal;
    }
}

} // namespace raikiri::venues::xt
