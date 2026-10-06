#pragma once

#include <string_view>

namespace raikiri::venues::upbit::ep {

inline constexpr std::string_view k_markets = "/v1/market/all";
inline constexpr std::string_view k_ticker = "/v1/ticker?markets=";
// no time method; the busiest book is restamped on every change
inline constexpr std::string_view k_server_time = "/v1/orderbook?markets=KRW-BTC&count=1";

} // namespace raikiri::venues::upbit::ep
