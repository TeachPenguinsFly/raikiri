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

// spot is another exchange on its own hosts; its socket speaks the contract socket's protocol
inline constexpr std::string_view k_spot_rest_host = "api-spot.weex.com";
inline constexpr std::string_view k_spot_stream_host = "ws-spot.weex.com";
inline constexpr std::string_view k_spot_exchange_info = "/api/v3/exchangeInfo";
// every symbol in one bare array, weight 2 of 500 per 10 s
inline constexpr std::string_view k_spot_ticker_24h = "/api/v3/market/ticker/24hr";
inline constexpr std::string_view k_spot_server_time = "/api/v3/time";
inline constexpr std::string_view k_spot_api_trading_symbols = "/api/v3/apiTradingSymbols";
// POST places, DELETE cancels and GET reads one order by origClientOrderId
inline constexpr std::string_view k_spot_order = "/api/v3/order";
// every symbol when none is named
inline constexpr std::string_view k_spot_open_orders = "/api/v3/openOrders";
// one symbol per call, up to 200 rows, windows up to 90 days
inline constexpr std::string_view k_spot_my_trades = "/api/v3/myTrades";
inline constexpr std::string_view k_spot_account = "/api/v3/account";
inline constexpr std::string_view k_spot_private_ws = "/v3/ws/private";

} // namespace raikiri::venues::weex::ep
