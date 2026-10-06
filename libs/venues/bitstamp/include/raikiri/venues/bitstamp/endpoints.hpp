#pragma once

#include <string_view>

namespace raikiri::venues::bitstamp::ep {

inline constexpr std::string_view k_host = "www.bitstamp.net";
inline constexpr std::string_view k_ws_host = "ws.bitstamp.net";
inline constexpr std::string_view k_markets = "/api/v2/markets/";
inline constexpr std::string_view k_tickers = "/api/v2/ticker/";
// no time method; the ticker is stamped to the second when served
inline constexpr std::string_view k_server_time = "/api/v2/ticker/btcusd/";

} // namespace raikiri::venues::bitstamp::ep
