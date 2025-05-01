#include <raikiri/venues/hyperliquid/errors.hpp>

namespace raikiri::venues::hyperliquid {

namespace {

[[nodiscard]] constexpr bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && is_space(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && is_space(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

} // namespace

bool is_null_body(std::string_view body) noexcept {
    return trim(body) == "null";
}

exec::ErrorClass classify(int status, std::string_view body) noexcept {
    // Status 0 is a transport failure: connectivity, not a venue verdict.
    if (status == 0) {
        return exec::ErrorClass::Connectivity;
    }
    if (status >= 200 && status < 300) {
        return is_null_body(body) ? exec::ErrorClass::Terminal : exec::ErrorClass::Retryable;
    }
    if (status == 429) {
        return exec::ErrorClass::RateLimited;
    }
    if (status >= 500) {
        return exec::ErrorClass::Retryable;
    }
    if (status == 401 || status == 403) {
        return exec::ErrorClass::AuthFailure;
    }
    // 400/405/415/422 and anything unrecognised: our request is wrong.
    return exec::ErrorClass::Terminal;
}

} // namespace raikiri::venues::hyperliquid
