#include <raikiri/venues/xt/errors.hpp>

#include <charconv>

namespace raikiri::venues::xt {

namespace {

// GATEWAY_000n is XT's risk control on both trees, and it is not an ordinary
// throttle: GATEWAY_0006 and _0007 lock the ACCOUNT for minutes or hours.
// Classifying it RateLimited is what makes the gateway back off instead of
// hammering its way into a longer lock.
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

    // The futures gateway answers with the HTTP status as the code. 403 covers
    // a failed signature, an unwhitelisted IP and an expired time window, and
    // 405 a null Host: all credential-shaped, and sending an operator to the
    // venue status page rather than to the key config costs an outage's worth
    // of time.
    //
    // 400 is deliberately NOT among them. XT uses it for a missing
    // validate-appkey header AND for a URL carrying a blacklisted keyword, and
    // the builder refuses to sign without credentials at all — so a 400 that
    // reaches here is the second case, and halting the venue over one
    // malformed request would take a healthy account down with it.
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
    // AUTH_001..007 are missing or malformed validate-* headers and
    // AUTH_101..106 are the key itself: nonexistent, inactive, mis-signed,
    // off-whitelist, stale, or short of permission. Every one of them is fixed
    // in the config and none of them by retrying.
    if (code.starts_with("AUTH_")) {
        return exec::ErrorClass::AuthFailure;
    }
    if (is_gateway(code)) {
        return exec::ErrorClass::RateLimited;
    }
    // COMMON_002 "system busy" and COMMON_003 "operation failed, try later" are
    // the only two codes XT itself describes as transient.
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
    // ORDER_005 "order not exist" on spot, and `invalid_order` on futures.
    // Futures `invalid_params` is deliberately absent: it names a missing order
    // AND a genuine parameter bug, and swallowing it would hide the second.
    return code == "ORDER_005" || code == "invalid_order";
}

} // namespace raikiri::venues::xt
