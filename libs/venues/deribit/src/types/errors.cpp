#include <raikiri/venues/deribit/errors.hpp>

namespace raikiri::venues::deribit {

namespace {

constexpr int k_http_ok_min = 200;
constexpr int k_http_ok_max = 299;
constexpr int k_http_too_many_requests = 429;
constexpr int k_http_server_error = 500;

} // namespace

exec::ErrorClass classify(std::int64_t code) noexcept {
    switch (code) {
    case k_invalid_credentials:
    case k_invalid_credentials_v1:
    case k_unauthorized:
    case k_forbidden:
    case k_permission_denied:
    case k_scope_exceeded:
    // The venue has taken the account away from us; retrying spends the rate
    // budget and changes nothing until a human intervenes.
    case k_account_locked:
        return exec::ErrorClass::AuthFailure;

    case k_too_many_requests:
    case k_too_many_concurrent_requests:
    case k_matching_engine_queue_full:
        return exec::ErrorClass::RateLimited;

    case k_retry:
    case k_settlement_in_progress:
    case k_temporarily_unavailable:
    case k_unavailable:
    case k_system_maintenance:
    case k_internal_server_error:
    case k_method_switched_off:
    case k_locked_by_admin:
    case k_unauthenticated_public_disabled:
    case k_timed_out:
        return exec::ErrorClass::Retryable;

    // The token expired mid-flight rather than never existing; the adapter
    // re-authenticates and the same order is worth sending again.
    case k_authorization_required:
        return exec::ErrorClass::Retryable;

    // A channel Deribit stopped serving.
    case k_subscribe_error_unsubscribed:
        return exec::ErrorClass::Retryable;

    default:
        return exec::ErrorClass::Terminal;
    }
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
        return exec::ErrorClass::Terminal; // the JSON-RPC envelope decides
    }
    return exec::ErrorClass::Terminal;
}

bool is_settled_order_refusal(std::int64_t code) noexcept {
    switch (code) {
    case k_order_not_found:
    case k_already_closed:
    case k_already_filled:
    case k_not_open_order:
    case k_edit_replaced:
        return true;
    default:
        return false;
    }
}

} // namespace raikiri::venues::deribit
