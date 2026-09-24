#pragma once

#include <string_view>

namespace raikiri::venues::weex::ep {

inline constexpr std::string_view k_rest_host = "api-contract.weex.com";
inline constexpr std::string_view k_stream_host = "ws-contract.weex.com";
inline constexpr std::string_view k_stream_port = "443";

// one public socket; /v3/ws/private is not opened since orders are answered over rest
inline constexpr std::string_view k_ws_public = "/v3/ws/public";

// assets, rateLimits and symbols in one large body; read once at start
inline constexpr std::string_view k_exchange_info = "/capi/v3/market/exchangeInfo";

// funding for every contract; weex pushes none of it on a socket
inline constexpr std::string_view k_premium_index = "/capi/v3/market/premiumIndex";

// read once at start; signatures more than 30 s off the venue clock are refused
inline constexpr std::string_view k_server_time = "/capi/v3/market/time";

// contracts orders may be sent for, a subset of the catalogue; the rest answer -1058
inline constexpr std::string_view k_api_trading_symbols = "/capi/v3/market/apiTradingSymbols";

// POST places one order, DELETE cancels one; the GET by orderId is unused
inline constexpr std::string_view k_order = "/capi/v3/order";

// DELETE cancels up to ten by orderIdList or origClientOrderIdList
inline constexpr std::string_view k_batch_orders = "/capi/v3/batchOrders";

// paged: limit maxes at 100, page counts from zero
inline constexpr std::string_view k_open_orders = "/capi/v3/openOrders";

// windowed: spans over 7 days are refused; no window means the last 7 days
inline constexpr std::string_view k_user_trades = "/capi/v3/userTrades";

// every open position, no parameters
inline constexpr std::string_view k_all_positions = "/capi/v3/account/position/allPosition";

// balances per coin, no parameters
inline constexpr std::string_view k_balance = "/capi/v3/account/balance";

// the account ledger, a POST with a body; funding is its position_funding rows
inline constexpr std::string_view k_income = "/capi/v3/account/income";

} // namespace raikiri::venues::weex::ep
