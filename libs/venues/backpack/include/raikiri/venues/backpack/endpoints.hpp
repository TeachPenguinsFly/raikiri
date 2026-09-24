#pragma once

#include <cstdint>
#include <string_view>

namespace raikiri::venues::backpack::ep {

// no testnet host resolves, so the flag is ignored
inline constexpr std::string_view k_rest_live = "api.backpack.exchange";
inline constexpr std::string_view k_stream_live = "ws.backpack.exchange";

[[nodiscard]] constexpr std::string_view rest_host(bool testnet) noexcept {
    (void)testnet;
    return k_rest_live;
}

[[nodiscard]] constexpr std::string_view stream_host(bool testnet) noexcept {
    (void)testnet;
    return k_stream_live;
}

// the bare host; /stream answers 400
inline constexpr std::string_view k_ws_path = "/";

inline constexpr std::string_view k_markets = "/api/v1/markets";
inline constexpr std::string_view k_depth = "/api/v1/depth";
inline constexpr std::string_view k_tickers = "/api/v1/tickers";

inline constexpr std::uint32_t k_requests_per_minute = 2000;

inline constexpr std::uint32_t k_request_cost = 1;

} // namespace raikiri::venues::backpack::ep
