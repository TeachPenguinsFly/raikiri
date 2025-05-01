#include <raikiri/venues/kucoin/errors.hpp>

namespace raikiri::venues::kucoin {

exec::ErrorClass classify(std::int64_t code) noexcept {
    switch (code) {
    case k_missing_credential_header:
    case k_api_key_not_found:
    case k_passphrase_error:
    case k_signature_error:
    case k_ip_not_whitelisted:
    case k_access_denied:
    case k_not_logged_in:
    case k_user_frozen:
        return exec::ErrorClass::AuthFailure;

    case k_too_many_requests:
        return exec::ErrorClass::RateLimited;

    case k_internal_server_error:
    case k_service_unavailable:
        return exec::ErrorClass::Retryable;

    case k_timestamp_invalid:
        return exec::ErrorClass::Retryable;

    case k_ws_token_invalid:
        return exec::ErrorClass::Retryable;

    default:
        return exec::ErrorClass::Terminal;
    }
}

bool is_expired_token(std::int64_t code) noexcept {
    return code == k_ws_token_invalid;
}

} // namespace raikiri::venues::kucoin
