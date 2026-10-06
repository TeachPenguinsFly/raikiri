#pragma once

// market data only: orders need a delegate key and the v3 session signer

#include <cstdint>
#include <string_view>

namespace raikiri::venues::vest::ep {

inline constexpr std::string_view k_rest_host = "server-prod.hz.vestmarkets.com";
inline constexpr std::string_view k_gateway_host = "api-gateway.hz.vestmarkets.com";
inline constexpr std::string_view k_ws_host = "ws.hz.vestmarkets.com";

inline constexpr std::string_view k_markets = "/v2/exchangeInfo";
inline constexpr std::string_view k_time = "/v2/exchangeInfo/ntp";
inline constexpr std::string_view k_tickers = "/v2/ticker/latest";
inline constexpr std::string_view k_volume = "/v3/ticker/24hr";
inline constexpr std::string_view k_open_interest = "/v3/oi";
inline constexpr std::string_view k_ws_path = "/ws?version=1.0&xwebsocketserver=restserver0";

inline constexpr std::uint64_t k_ticker_interval_ns = 2'000'000'000ULL;
inline constexpr std::uint64_t k_stats_interval_ns = 60'000'000'000ULL;
inline constexpr std::uint64_t k_app_ping_interval_ns = 20'000'000'000ULL;

inline constexpr std::string_view k_ping_frame = R"({"method":"PING","params":[],"id":0})";

} // namespace raikiri::venues::vest::ep
