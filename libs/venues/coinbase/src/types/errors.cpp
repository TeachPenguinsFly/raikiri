#include <raikiri/venues/coinbase/errors.hpp>

namespace raikiri::venues::coinbase {

exec::ErrorClass classify_http(int status) noexcept {
    if (status == 0) {
        return exec::ErrorClass::Connectivity;
    }
    if (status == 429) {
        return exec::ErrorClass::RateLimited;
    }
    if (status == 401 || status == 403) {
        return exec::ErrorClass::AuthFailure;
    }
    if (status == 408 || status >= 500) {
        return exec::ErrorClass::Retryable;
    }
    return exec::ErrorClass::Terminal;
}

exec::ErrorClass classify_stream(std::string_view message) noexcept {
    if (message.starts_with("Rate limit")) {
        return exec::ErrorClass::RateLimited;
    }
    return exec::ErrorClass::Terminal;
}

} // namespace raikiri::venues::coinbase
