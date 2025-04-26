#pragma once

// Bitget error-code classification. A code in the wrong bucket is a trading
// bug; unknown codes default to Terminal, because a refusal repeated at full
// speed is how an account earns an IP ban.
//
// REST codes are STRINGS and success is "00000", so classification compares
// text: converting first collapses "00000", "0" and an unreadable field onto
// the same integer, and only one of those is success. Websocket codes are a
// different space AND a different type — unquoted numbers — and are recorded
// rather than classified, since a refused subscription is terminal for the
// channel it names whatever the code says.

#include <raikiri/exec/error.hpp>

#include <cstdint>
#include <string_view>

namespace raikiri::venues::bitget {

// The first four VERIFIED live on 2026-08-11; the rest from the v2 reference.
inline constexpr std::string_view k_invalid_access_key = "40006";
inline constexpr std::string_view k_parameter_not_exist = "40034";
inline constexpr std::string_view k_parameter_check_failed = "400172";
inline constexpr std::string_view k_url_not_found = "40404";
inline constexpr std::string_view k_access_key_empty = "40001";
inline constexpr std::string_view k_secret_key_empty = "40002";
inline constexpr std::string_view k_signature_empty = "40003";
inline constexpr std::string_view k_timestamp_expired = "40008";
inline constexpr std::string_view k_signature_error = "40009";
inline constexpr std::string_view k_parameter_verification = "40017";
inline constexpr std::string_view k_system_error = "25000";
inline constexpr std::string_view k_too_many_requests = "429";

// VERIFIED live against wss://ws.bitget.com/v2/ws/public.
inline constexpr std::int64_t k_ws_unknown_subscription = 30001;
inline constexpr std::int64_t k_ws_unrecognized_request = 30002;
inline constexpr std::int64_t k_ws_param_error = 30016;

[[nodiscard]] exec::ErrorClass classify(std::string_view code) noexcept;

// Numeric form for exec::VenueError's int64 field. A non-numeric code yields
// 0, which collides with "00000" — classification never reads this value.
[[nodiscard]] std::int64_t code_number(std::string_view code) noexcept;

} // namespace raikiri::venues::bitget
