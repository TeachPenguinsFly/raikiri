#include <raikiri/venues/htx/errors.hpp>

#include <charconv>

namespace raikiri::venues::htx {

namespace {

constexpr int k_http_ok_min = 200;
constexpr int k_http_ok_max = 299;
constexpr int k_http_too_many_requests = 429;
constexpr int k_http_server_error = 500;

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
        if (number >= k_swap_auth_detail_first && number <= k_swap_auth_detail_last) {
            return exec::ErrorClass::AuthFailure;
        }
        switch (number) {
        case k_signature_rejected:
            return exec::ErrorClass::AuthFailure;
        case k_swap_redis_error:
        case k_swap_system_busy:
        case k_swap_service_abnormal:
        case k_swap_service_unavailable:
            return exec::ErrorClass::Retryable;
        case k_swap_rate_limited:
        case k_swap_cancel_ratio_ban:
        case k_too_many_requests:
            return exec::ErrorClass::RateLimited;
        default:
            return exec::ErrorClass::Terminal;
        }
    }
    if (code == k_spot_login_required || code == k_spot_signature_invalid) {
        return exec::ErrorClass::AuthFailure;
    }
    return exec::ErrorClass::Terminal;
}

bool is_benign_cancel_failure(std::string_view code) noexcept {
    std::int64_t number = 0;
    if (as_number(code, number)) {
        return number == k_swap_nothing_to_cancel || number == k_swap_order_missing ||
               number == k_swap_cancel_in_flight || number == k_swap_already_executed ||
               number == k_swap_already_cancelled;
    }
    return code == k_spot_order_state || code == k_spot_record_invalid ||
           code == k_spot_not_found || code == k_spot_base_not_found ||
           code == k_client_id_not_found;
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
