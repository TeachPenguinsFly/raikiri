#include <raikiri/venues/xt/errors.hpp>

#include <charconv>

namespace raikiri::venues::xt {

namespace {

// GATEWAY_000n is risk control and _0006/_0007 lock the account, so it is RateLimited
[[nodiscard]] bool is_gateway(std::string_view code) noexcept {
    return code.starts_with("GATEWAY_");
}

} // namespace

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
    if (is_gateway(code)) {
        return exec::ErrorClass::RateLimited;
    }

    // 403 and 405 are credential errors; 400 also means a blacklisted url keyword, so it does not halt
    switch (numeric_code(code)) {
    case 401:
    case 403:
    case 405:
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

exec::ErrorClass classify_spot(std::string_view code) noexcept {
    // AUTH_001..007 are header faults and AUTH_101..106 key faults; both are config fixes
    if (code.starts_with("AUTH_")) {
        return exec::ErrorClass::AuthFailure;
    }
    if (is_gateway(code)) {
        return exec::ErrorClass::RateLimited;
    }
    // the only two codes xt documents as transient
    if (code == "COMMON_002" || code == "COMMON_003") {
        return exec::ErrorClass::Retryable;
    }
    switch (numeric_code(code)) {
    case 429:
        return exec::ErrorClass::RateLimited;
    case 500:
    case 502:
    case 503:
        return exec::ErrorClass::Retryable;
    default:
        return exec::ErrorClass::Terminal;
    }
}

bool is_benign_cancel_failure(std::string_view code) noexcept {
    // invalid_params is excluded: it also covers real parameter bugs
    return code == "ORDER_005" || code == "invalid_order";
}

} // namespace raikiri::venues::xt
