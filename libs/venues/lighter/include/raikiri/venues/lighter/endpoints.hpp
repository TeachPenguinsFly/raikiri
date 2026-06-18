#pragma once

#include <cstdint>
#include <string_view>

namespace raikiri::venues::lighter::ep {

inline constexpr std::string_view k_rest_live = "mainnet.zklighter.elliot.ai";
inline constexpr std::string_view k_rest_test = "testnet.zklighter.elliot.ai";

[[nodiscard]] constexpr std::string_view rest_host(bool testnet) noexcept {
    return testnet ? k_rest_test : k_rest_live;
}

[[nodiscard]] constexpr std::string_view stream_host(bool testnet) noexcept {
    return rest_host(testnet);
}

inline constexpr std::string_view k_ws_stream = "/stream";

// Perp and spot in one body, with the funding clamps /api/v1/orderBooks lacks.
inline constexpr std::string_view k_order_book_details = "/api/v1/orderBookDetails";

// Not read: a comparison table restating every rate per 8 h, other venues' rates
// under lighter market ids.
inline constexpr std::string_view k_funding_comparison = "/api/v1/funding-rates";

// A REST 429 throttles the same IP's sockets too, so REST fetches only the catalogue.
inline constexpr std::uint32_t k_rest_requests_per_minute = 60;
inline constexpr std::uint64_t k_rest_cooldown_ns = 60ULL * 1'000'000'000ULL;

// The venue takes 200 socket messages a minute per IP and refuses the rest with 30009.
// A perp and a spot gateway on one host share it; RAIKIRI_LIGHTER_WS_PER_MINUTE
// splits it between them.
inline constexpr std::uint32_t k_ws_messages_per_minute = 180;
inline constexpr std::string_view k_ws_messages_env = "RAIKIRI_LIGHTER_WS_PER_MINUTE";
inline constexpr std::int64_t k_ws_rate_limited = 30009;
inline constexpr std::uint64_t k_ws_refusal_hold_ns = 10ULL * 1'000'000'000ULL;

} // namespace raikiri::venues::lighter::ep
