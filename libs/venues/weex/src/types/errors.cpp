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
        // Wrong for every request this process will make, so the gateway halts
        // rather than spending a budget on a key the venue keeps refusing.
        return exec::ErrorClass::AuthFailure;

    case k_order_rate_limited:
        // The venue bans for ten seconds after a breach, so this must back off
        // rather than repeat.
        return exec::ErrorClass::RateLimited;

    case k_system_error:
        return exec::ErrorClass::Retryable;

    case k_symbol_not_api_tradable:
    case k_key_not_bound_to_symbol:
    case k_contract_no_permission_pair:
        // NAMED RATHER THAN LEFT TO THE DEFAULT, because each of these reads as
        // a permission and none of them is one the gateway may halt on: they
        // are about ONE contract, and the other nine hundred still trade.
        return exec::ErrorClass::Terminal;

    default:
        break;
    }
    // Everything else — a malformed request, a rejected order, a quantity the
    // venue would not take — is a refusal that asking again reproduces exactly.
    return exec::ErrorClass::Terminal;
}

bool is_benign_cancel_failure(std::int64_t code) noexcept {
    return code == k_order_not_found;
}

} // namespace raikiri::venues::weex
