#include <raikiri/venues/weex/errors.hpp>

namespace raikiri::venues::weex {

exec::ErrorClass classify(std::int64_t code) noexcept {
    switch (code) {
    case k_access_key_empty:
    case k_access_sign_empty:
    case k_access_timestamp_empty:
    case k_access_timestamp_invalid:
    case k_access_key_invalid:
    case k_access_timestamp_expired:
    case k_auth_failed:
    case k_key_or_passphrase_wrong:
    case k_user_forbidden:
    case k_permission_denied:
    case k_insufficient_permissions:
    case k_permission_check_failed:
    case k_user_auth_not_safe:
    case k_illegal_ip:
    case k_user_locked:
    case k_access_forbidden:
        // a credential error applies to every request, so halt
        return exec::ErrorClass::AuthFailure;

    case k_order_rate_limited:
        // weex bans for ten seconds after a breach
        return exec::ErrorClass::RateLimited;

    case k_system_error:
        return exec::ErrorClass::Retryable;

    case k_symbol_not_api_tradable:
    case k_key_not_bound_to_symbol:
    case k_contract_no_permission_pair:
        // per-contract refusals; halting would silence every other contract
        return exec::ErrorClass::Terminal;

    default:
        break;
    }
    return exec::ErrorClass::Terminal;
}

bool is_benign_cancel_failure(std::int64_t code) noexcept {
    return code == k_order_not_found;
}

} // namespace raikiri::venues::weex
