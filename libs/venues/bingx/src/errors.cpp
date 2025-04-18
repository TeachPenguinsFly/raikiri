#include <raikiri/venues/bingx/errors.hpp>

namespace raikiri::venues::bingx {

namespace {

constexpr int k_http_ok_min = 200;
constexpr int k_http_ok_max = 299;
constexpr int k_http_too_many_requests = 429;
constexpr int k_http_server_error = 500;

} // namespace

exec::ErrorClass classify(std::int64_t code) noexcept {
    if (code == k_bad_api_key) {
        return exec::ErrorClass::AuthFailure;
    }
    // Everything else known lands on the default anyway: a refused
    // subscription, an unlisted symbol, a rejected parameter and a missing path
    // are all permanent for the request that caused them, and retrying
    // reproduces them exactly.
    return exec::ErrorClass::Terminal;
}

exec::ErrorClass classify_http(int status) noexcept {
    if (status == 0) {
        return exec::ErrorClass::Connectivity; // never reached the venue
    }
    if (status == k_http_too_many_requests) {
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
