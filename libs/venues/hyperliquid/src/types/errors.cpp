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
    // status 0 is a transport failure
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
    return exec::ErrorClass::Terminal;
}

exec::ErrorClass classify_reason(std::string_view reason) noexcept {
    const auto text = trim(reason);
    if (text.empty()) {
        return exec::ErrorClass::Terminal;
    }
    // the venue's limiter is priced in traded volume
    if (text.starts_with("Too many cumulative requests")) {
        return exec::ErrorClass::RateLimited;
    }
    // a malformed or unknown signature; retrying cannot help
    if (text.starts_with("Unable to recover signer") || text.starts_with("User or API Wallet") ||
        text.starts_with("L1 error: User or API Wallet")) {
        return exec::ErrorClass::AuthFailure;
    }
    // halt until the account is funded
    if (text.starts_with("Must deposit before performing actions")) {
        return exec::ErrorClass::AuthFailure;
    }
    // remaining refusals are properties of the order and repeat on resend
    return exec::ErrorClass::Terminal;
}

bool is_benign_cancel_failure(std::string_view reason) noexcept {
    const auto text = trim(reason);
    return text.starts_with("Order was never placed, already canceled, or filled") ||
           text.starts_with("TWAP was never placed, already canceled, or filled");
}

bool is_ioc_miss(std::string_view reason) noexcept {
    return trim(reason).starts_with("Order could not immediately match against any resting orders");
}

} // namespace raikiri::venues::hyperliquid
