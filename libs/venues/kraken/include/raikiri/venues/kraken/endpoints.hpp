#pragma once

// paths are the signed form; k_route_prefix is prepended to the target and never signed

#include <string_view>

namespace raikiri::venues::kraken::ep {

// one host for rest and the socket; there is no demo host
inline constexpr std::string_view k_rest = "futures.kraken.com";
inline constexpr std::string_view k_stream = "futures.kraken.com";
// the prefix every rest target carries and no signature does
inline constexpr std::string_view k_route_prefix = "/derivatives";

// private feeds use this socket after a challenge
inline constexpr std::string_view k_ws = "/ws/v1";

inline constexpr std::string_view k_send_order = "/api/v3/sendorder";
inline constexpr std::string_view k_cancel_order = "/api/v3/cancelorder";
inline constexpr std::string_view k_edit_order = "/api/v3/editorder";

inline constexpr std::string_view k_open_orders = "/api/v3/openorders";
inline constexpr std::string_view k_fills = "/api/v3/fills";
inline constexpr std::string_view k_open_positions = "/api/v3/openpositions";

// no clock endpoint; every response carries serverTime
inline constexpr std::string_view k_instruments = "/api/v3/instruments";
inline constexpr std::string_view k_tickers = "/api/v3/tickers";
inline constexpr std::string_view k_orderbook = "/api/v3/orderbook";

} // namespace raikiri::venues::kraken::ep
