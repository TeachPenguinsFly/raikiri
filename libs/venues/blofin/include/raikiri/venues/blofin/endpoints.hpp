#pragma once

#include <cstdint>
#include <string_view>

namespace raikiri::venues::blofin::ep {

inline constexpr std::string_view k_host_live = "openapi.blofin.com";
inline constexpr std::string_view k_host_demo = "demo-trading-openapi.blofin.com";

[[nodiscard]] constexpr std::string_view host(bool testnet) noexcept {
    return testnet ? k_host_demo : k_host_live;
}

inline constexpr std::string_view k_ws_public = "/ws/public";

inline constexpr std::string_view k_instruments = "/api/v1/market/instruments";
inline constexpr std::string_view k_funding_rate = "/api/v1/market/funding-rate";
inline constexpr std::string_view k_mark_price = "/api/v1/market/mark-price";

// per ip and unweighted; no header reports what is left
inline constexpr std::uint32_t k_rest_requests_per_minute = 500;
inline constexpr std::uint32_t k_rest_requests_per_5_minutes = 1500;
// tripping the minute suspends the ip for five minutes, the five-minute window for an hour
inline constexpr std::uint64_t k_rest_suspension_ns = 5ULL * 60 * 1'000'000'000ULL;

} // namespace raikiri::venues::blofin::ep
