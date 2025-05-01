#include <raikiri/venues/htx/errors.hpp>

#include <charconv>

namespace raikiri::venues::htx {

namespace {

constexpr int k_http_ok_min = 200;
constexpr int k_http_ok_max = 299;
constexpr int k_http_too_many_requests = 429;
constexpr int k_http_server_error = 500;

// Returns false for a word code, which is how the two spellings are told
// apart without the caller having to know which surface it came from.
bool as_number(std::string_view code, std::int64_t& out) noexcept {
    if (code.empty()) {
        return false;
    }
    const auto* const end = code.data() + code.size();
    const auto [stop, ec] = std::from_chars(code.data(), end, out);
    return ec == std::errc{} && stop == end;
}

} // namespace

exec::ErrorClass classify(std::string_view code) noexcept {
    std::int64_t number = 0;
    if (as_number(code, number)) {
        if (number == k_signature_rejected) {
            return exec::ErrorClass::AuthFailure;
        }
        return exec::ErrorClass::Terminal;
    }
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

} // namespace raikiri::venues::htx
