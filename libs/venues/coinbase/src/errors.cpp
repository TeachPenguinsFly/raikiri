#include <raikiri/venues/coinbase/errors.hpp>

namespace raikiri::venues::coinbase {

exec::ErrorClass classify_http(int status) noexcept {
    // Status 0 is RestClient's marker for a failure that happened before any
    // HTTP verdict existed, which is a different thing from a venue refusal.
    if (status == 0) {
        return exec::ErrorClass::Connectivity;
    }
    if (status == 429) {
        return exec::ErrorClass::RateLimited;
    }
    if (status == 401 || status == 403) {
        return exec::ErrorClass::AuthFailure;
    }
    // 408 and 504 are timeouts wearing different numbers; both mean the
    // request may simply not have reached a decision yet.
    if (status == 408 || status >= 500) {
        return exec::ErrorClass::Retryable;
    }
    return exec::ErrorClass::Terminal;
}

exec::ErrorClass classify_stream(std::string_view message) noexcept {
    // Coinbase states its websocket rate limit as a message rather than a
    // close code, so the text is the only signal there is.
    if (message.starts_with("Rate limit")) {
        return exec::ErrorClass::RateLimited;
    }
    return exec::ErrorClass::Terminal;
}

} // namespace raikiri::venues::coinbase
