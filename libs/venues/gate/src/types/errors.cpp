#include <raikiri/venues/gate/errors.hpp>

#include <algorithm>
#include <array>

namespace raikiri::venues::gate {

namespace {

struct LabelClass {
    std::string_view label;
    exec::ErrorClass cls;
};

// Only the labels that are NOT Terminal need listing: everything else falls
// through to the default, which is what makes an unknown refusal safe.
constexpr std::array<LabelClass, 17> k_table{{
    {k_authentication_failed, exec::ErrorClass::AuthFailure},
    {k_invalid_credentials, exec::ErrorClass::AuthFailure},
    {k_invalid_key, exec::ErrorClass::AuthFailure},
    {k_invalid_signature, exec::ErrorClass::AuthFailure},
    {k_ip_forbidden, exec::ErrorClass::AuthFailure},
    {k_read_only, exec::ErrorClass::AuthFailure},
    {k_forbidden, exec::ErrorClass::AuthFailure},
    {k_account_locked, exec::ErrorClass::AuthFailure},
    {k_missing_required_header, exec::ErrorClass::AuthFailure},
    {k_trade_restricted, exec::ErrorClass::AuthFailure},
    {k_too_fast, exec::ErrorClass::RateLimited},
    {k_too_many_requests, exec::ErrorClass::RateLimited},
    // REQUEST_EXPIRED is deliberately absent here and handled below.
    {k_internal, exec::ErrorClass::Retryable},
    {k_order_pending, exec::ErrorClass::Retryable},
    {k_position_in_liquidation, exec::ErrorClass::Retryable},
    {k_position_in_close, exec::ErrorClass::Retryable},
    // The order was placed and only the answer went missing; resending it
    // would be a second order, so this is the venue telling us to stop.
    {k_duplicate_request, exec::ErrorClass::Terminal},
}};

} // namespace

exec::ErrorClass classify(std::string_view label) noexcept {
    if (label == k_request_expired) {
        return exec::ErrorClass::Retryable;
    }
    if (label == k_server_error || label == k_too_busy) {
        return exec::ErrorClass::Retryable;
    }
    const auto* const found = std::ranges::find_if(
        k_table, [label](const LabelClass& entry) { return entry.label == label; });
    return found != k_table.end() ? found->cls : exec::ErrorClass::Terminal;
}

exec::ErrorClass classify_stream(std::int64_t code) noexcept {
    switch (code) {
    case k_ws_auth_failed:
    case k_ws_invalid_key:
        return exec::ErrorClass::AuthFailure;
    case k_ws_not_logged_in:
    case k_ws_server_error:
        return exec::ErrorClass::Retryable;
    default:
        return exec::ErrorClass::Terminal;
    }
}

bool is_benign_cancel_failure(std::string_view label) noexcept {
    return label == k_order_not_found || label == k_client_id_not_found ||
           label == k_order_closed || label == k_order_cancelled || label == k_order_finished;
}

} // namespace raikiri::venues::gate
