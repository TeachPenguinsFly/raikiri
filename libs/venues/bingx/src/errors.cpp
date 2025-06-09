#include <raikiri/venues/bingx/errors.hpp>

namespace raikiri::venues::bingx {

namespace {

constexpr int k_http_ok_min = 200;
constexpr int k_http_ok_max = 299;
constexpr int k_http_too_many_requests = 429;
constexpr int k_http_ip_banned = 418;
constexpr int k_http_server_error = 500;

} // namespace

exec::ErrorClass classify(std::int64_t code) noexcept {
    switch (code) {
    // The key, the signature or the account's standing. Halting is the point:
    // a permission refusal retried at full speed is how a restricted key
    // becomes a banned IP.
    case k_bad_signature:
    case k_no_permission:
    case k_missing_signature:
    case k_bad_api_key:
    case k_ip_not_whitelisted:
    case k_spot_account_restricted:
    case k_region_restricted:
    case k_kyc_required:
    case k_region_or_kyc:
        return exec::ErrorClass::AuthFailure;

    case k_rate_limited:
    case k_risk_control:
    case k_too_many_invalid:
        return exec::ErrorClass::RateLimited;

    // Each of these has a remedy that is simply waiting: the clock resync, the
    // settlement window closing, the venue's own queue draining.
    case k_bad_timestamp:
    case k_system_busy:
    case k_funding_settling:
    case k_assets_processing:
    case k_account_changed:
    case k_internal_error:
    case k_network_exception:
    case k_order_system_busy:
        return exec::ErrorClass::Retryable;

    default:
        break;
    }
    return exec::ErrorClass::Terminal;
}

bool is_benign_cancel_failure(std::int64_t code) noexcept {
    return code == k_order_not_found || code == k_spot_order_not_found;
}

exec::ErrorClass classify_http(int status) noexcept {
    if (status == 0) {
        return exec::ErrorClass::Connectivity; // never reached the venue
    }
    if (status == k_http_too_many_requests || status == k_http_ip_banned) {
        return exec::ErrorClass::RateLimited;
    }
    if (status >= k_http_server_error) {
        return exec::ErrorClass::Retryable;
    }
    if (status >= k_http_ok_min && status <= k_http_ok_max) {
        return exec::ErrorClass::Terminal; // the envelope decides, not the status
    }
    return exec::ErrorClass::Terminal;
}

} // namespace raikiri::venues::bingx
