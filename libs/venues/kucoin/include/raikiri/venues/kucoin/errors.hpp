#pragma once

// unknown codes are terminal, since retrying an unknown refusal at full speed risks an ip ban

#include <raikiri/exec/error.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/wire/error.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace raikiri::venues::kucoin {

using WireError = wire::Error;

// rest; 200000 is success
inline constexpr std::int64_t k_missing_credential_header = 400001;
inline constexpr std::int64_t k_timestamp_invalid = 400002; // >5 s from server time
inline constexpr std::int64_t k_api_key_not_found = 400003;
inline constexpr std::int64_t k_passphrase_error = 400004;
inline constexpr std::int64_t k_signature_error = 400005;
inline constexpr std::int64_t k_ip_not_whitelisted = 400006;
inline constexpr std::int64_t k_access_denied = 400007;
inline constexpr std::int64_t k_parameter_error = 400100;
inline constexpr std::int64_t k_order_forbidden = 400200;
inline constexpr std::int64_t k_not_logged_in = 400401;
inline constexpr std::int64_t k_validation_failed = 400600;
inline constexpr std::int64_t k_interface_deprecated = 401000;
inline constexpr std::int64_t k_url_not_found = 404000; // unknown symbol
inline constexpr std::int64_t k_network_timeout = 408000;
inline constexpr std::int64_t k_user_frozen = 411100;
inline constexpr std::int64_t k_unsupported = 415000; // no funding rate
inline constexpr std::int64_t k_too_many_requests = 429000;
inline constexpr std::int64_t k_internal_server_error = 500000;
inline constexpr std::int64_t k_service_unavailable = 503000;

// spot and futures publish separate code tables that do not overlap
inline constexpr std::int64_t k_spot_order_count_exceeded = 200003;
inline constexpr std::int64_t k_spot_balance_insufficient = 200004;
inline constexpr std::int64_t k_spot_symbol_not_tradable = 600203;
inline constexpr std::int64_t k_spot_symbol_unknown = 900001;

inline constexpr std::int64_t k_futures_order_not_cancelable = 100004;
inline constexpr std::int64_t k_futures_balance_insufficient = 300003;
inline constexpr std::int64_t k_futures_no_position_to_close = 300009;
inline constexpr std::int64_t k_futures_leverage_too_high = 300016;
inline constexpr std::int64_t k_futures_client_oid_repeated = 300018;

// websocket
inline constexpr std::int64_t k_ws_token_invalid = 401;
inline constexpr std::int64_t k_ws_topic_not_found = 404;

[[nodiscard]] exec::ErrorClass classify(std::int64_t code) noexcept;

// an expired bullet token needs a new handshake; the socket's own backoff would reuse it
[[nodiscard]] bool is_expired_token(std::int64_t code) noexcept;

// a cancel that lost the race to a fill is benign; kucoin says so only in the 400100 text
[[nodiscard]] bool is_benign_cancel_failure(std::int64_t code, std::string_view msg) noexcept;

// how long a 429 holds the allowance: gw-ratelimit-reset ms, else a second
[[nodiscard]] std::uint64_t refusal_hold_ms(std::span<const net::RateHeader> headers) noexcept;

} // namespace raikiri::venues::kucoin
