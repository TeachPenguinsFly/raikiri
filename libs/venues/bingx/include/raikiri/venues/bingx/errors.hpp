#pragma once

// unknown codes default to Terminal
// refusals arrive as http 200 with the code in the body, except userDataStream which uses http statuses

#include <raikiri/exec/error.hpp>
#include <raikiri/wire/error.hpp>

#include <cstdint>

namespace raikiri::venues::bingx {

using WireError = wire::Error;

// an unlisted symbol and an unknown dataType share this code
inline constexpr std::int64_t k_ws_data_type_unsupported = 80015;

inline constexpr std::int64_t k_bad_signature = 100001;

inline constexpr std::int64_t k_no_permission = 100004;

// answers with timestamp instead of data
inline constexpr std::int64_t k_endpoint_not_found = 100400;

// spot: a cancel that lost the race with a fill
inline constexpr std::int64_t k_spot_order_not_found = 100404;

inline constexpr std::int64_t k_rate_limited = 100410;

inline constexpr std::int64_t k_missing_signature = 100412;

// missing or wrong X-BX-APIKEY
inline constexpr std::int64_t k_bad_api_key = 100413;

inline constexpr std::int64_t k_ip_not_whitelisted = 100419;

// retryable: the adapter resyncs its clock offset
inline constexpr std::int64_t k_bad_timestamp = 100421;

inline constexpr std::int64_t k_system_busy = 100500;

inline constexpr std::int64_t k_spot_account_restricted = 100441;

// funding settlement in progress; clears in seconds
inline constexpr std::int64_t k_funding_settling = 101214;
inline constexpr std::int64_t k_assets_processing = 101219;
inline constexpr std::int64_t k_account_changed = 101412;
inline constexpr std::int64_t k_region_restricted = 101210;
inline constexpr std::int64_t k_kyc_required = 101483;
inline constexpr std::int64_t k_region_or_kyc = 101484;

// also answers /quote/depth with a limit outside 5/10/20/50/100/500/1000
inline constexpr std::int64_t k_invalid_parameter = 109400;

// request rate or cancel-to-fill ratio limit
inline constexpr std::int64_t k_risk_control = 109403;
inline constexpr std::int64_t k_too_many_invalid = 109429;

// perp: order filled, cancelled or unknown
inline constexpr std::int64_t k_order_not_found = 109421;

inline constexpr std::int64_t k_symbol_not_found = 109425;

inline constexpr std::int64_t k_internal_error = 109500;
inline constexpr std::int64_t k_network_exception = 109701;
inline constexpr std::int64_t k_order_system_busy = 110500;

[[nodiscard]] exec::ErrorClass classify(std::int64_t code) noexcept;

// the order already finished; reconciliation settles it
[[nodiscard]] bool is_benign_cancel_failure(std::int64_t code) noexcept;

// 429 and 5xx arrive before any envelope
[[nodiscard]] exec::ErrorClass classify_http(int status) noexcept;

} // namespace raikiri::venues::bingx
