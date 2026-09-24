#pragma once

#include <string_view>

namespace raikiri::venues::xt::ep {

// xt's demo host is spot-only and does not serve fstream
inline constexpr std::string_view k_rest_futures = "fapi.xt.com";
inline constexpr std::string_view k_rest_spot = "sapi.xt.com";
inline constexpr std::string_view k_stream_futures = "fstream.xt.com";
// spot serves public and private sockets from this host
inline constexpr std::string_view k_stream_spot = "stream.xt.com";

inline constexpr std::string_view k_ws_futures_market = "/ws/market";
inline constexpr std::string_view k_ws_futures_user = "/ws/user";
inline constexpr std::string_view k_ws_spot_public = "/public";
inline constexpr std::string_view k_ws_spot_private = "/private";

inline constexpr std::string_view k_futures_order_create = "/future/trade/v1/order/create";
inline constexpr std::string_view k_futures_order_cancel = "/future/trade/v1/order/cancel";
inline constexpr std::string_view k_futures_order_update = "/future/trade/v1/order/update";
// DELETE and PUT put the venue order id in the path, so it is part of the signed path
inline constexpr std::string_view k_spot_order = "/v4/order";

inline constexpr std::string_view k_futures_open_orders = "/future/trade/v1/order/list-open-order";
inline constexpr std::string_view k_futures_trades = "/future/trade/v1/order/trade-list";
inline constexpr std::string_view k_futures_positions = "/future/user/v1/position/list";
// u-based wallet; coin-margined lives on another host
inline constexpr std::string_view k_futures_balances = "/future/user/v1/balance/list";
// startTime filters; endTime is refused with returnCode 1
inline constexpr std::string_view k_futures_bills = "/future/user/v1/balance/bills";
inline constexpr std::string_view k_spot_open_orders = "/v4/open-order";
inline constexpr std::string_view k_spot_trades = "/v4/trade";
// futures mints a listen key, spot a ws token
inline constexpr std::string_view k_futures_listen_key = "/future/user/v1/user/listen-key";
inline constexpr std::string_view k_spot_ws_token = "/v4/ws-token";

inline constexpr std::string_view k_futures_symbols = "/future/market/v1/public/symbol/list";
inline constexpr std::string_view k_spot_symbols = "/v4/public/symbol";
inline constexpr std::string_view k_futures_server_time = "/future/market/v1/public/time";
inline constexpr std::string_view k_spot_server_time = "/v4/public/time";
inline constexpr std::string_view k_futures_funding_rate =
    "/future/market/v1/public/q/funding-rate";
inline constexpr std::string_view k_futures_depth = "/future/market/v1/public/q/depth";

} // namespace raikiri::venues::xt::ep
