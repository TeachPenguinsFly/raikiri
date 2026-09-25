#pragma once

// futures public data, signed calls and the spot socket each use a different host

#include <string_view>

namespace raikiri::venues::mexc::ep {

// mexc runs no testnet
inline constexpr std::string_view k_rest_public = "contract.mexc.com";
// every signed call; the public host 403s a private post
inline constexpr std::string_view k_rest_private = "api.mexc.com";
inline constexpr std::string_view k_stream_futures = "contract.mexc.com";
inline constexpr std::string_view k_stream_spot = "wbs-api.mexc.com";

// the futures socket also carries the private stream after login
inline constexpr std::string_view k_ws_futures = "/edge";
inline constexpr std::string_view k_ws_spot = "/ws";

inline constexpr std::string_view k_futures_order_create = "/api/v1/private/order/create";
// takes an array of venue ids; the next path cancels one order by client id
inline constexpr std::string_view k_futures_order_cancel = "/api/v1/private/order/cancel";
inline constexpr std::string_view k_futures_order_cancel_by_client_id = "/api/v1/private/order/cancel_with_external";
// post places, delete cancels, get reads
inline constexpr std::string_view k_spot_order = "/api/v3/order";

// an optional symbol follows as a trailing path segment
inline constexpr std::string_view k_futures_open_orders = "/api/v1/private/order/list/open_orders";
// Takes /{symbol}/{externalOid} after it.
inline constexpr std::string_view k_futures_order_by_client_id = "/api/v1/private/order/external";
inline constexpr std::string_view k_futures_order_deals = "/api/v1/private/order/list/order_deals";
inline constexpr std::string_view k_futures_positions = "/api/v1/private/position/open_positions";
// `symbol` is optional here
inline constexpr std::string_view k_futures_funding_records = "/api/v1/private/position/funding_records";
inline constexpr std::string_view k_spot_open_orders = "/api/v3/openOrders";
inline constexpr std::string_view k_spot_my_trades = "/api/v3/myTrades";

inline constexpr std::string_view k_contract_detail = "/api/v1/contract/detail";
inline constexpr std::string_view k_exchange_info = "/api/v3/exchangeInfo";
// spot signs against its own deployment's clock
inline constexpr std::string_view k_futures_server_time = "/api/v1/contract/ping";
inline constexpr std::string_view k_spot_server_time = "/api/v3/time";
inline constexpr std::string_view k_spot_ticker_24h = "/api/v3/ticker/24hr";
// the symbol is a trailing path segment on these two and a query parameter on the ticker
inline constexpr std::string_view k_contract_depth = "/api/v1/contract/depth";
inline constexpr std::string_view k_contract_funding_rate = "/api/v1/contract/funding_rate";
inline constexpr std::string_view k_contract_ticker = "/api/v1/contract/ticker";

} // namespace raikiri::venues::mexc::ep
