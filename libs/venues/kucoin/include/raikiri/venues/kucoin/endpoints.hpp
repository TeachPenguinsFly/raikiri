#pragma once

#include <string_view>

namespace raikiri::venues::kucoin::ep {

// the futures host also serves market data; kucoin has no testnet
inline constexpr std::string_view k_rest_futures = "api-futures.kucoin.com";
inline constexpr std::string_view k_rest_spot = "api.kucoin.com";

// the bullet call returns the socket host and target; the host it is called on picks the account stream
inline constexpr std::string_view k_bullet_public = "/api/v1/bullet-public";
inline constexpr std::string_view k_bullet_private = "/api/v1/bullet-private";

// order entry for spot and futures alike; the key, timestamp and signatures ride in the query
inline constexpr std::string_view k_ws_trade_host = "wsapi.kucoin.com";
inline constexpr std::string_view k_ws_trade_path = "/v1/private";

inline constexpr std::string_view k_spot_orders = "/api/v1/hf/orders";
inline constexpr std::string_view k_spot_order_amend = "/api/v1/hf/orders/alter";
inline constexpr std::string_view k_futures_orders = "/api/v1/orders";
// cancel by client id: .../orders/client-order/{clientOid}
inline constexpr std::string_view k_client_order_segment = "client-order/";

inline constexpr std::string_view k_spot_orders_active = "/api/v1/hf/orders/active";
inline constexpr std::string_view k_spot_active_symbols = "/api/v1/hf/orders/active/symbols";
inline constexpr std::string_view k_spot_fills = "/api/v1/hf/fills";
inline constexpr std::string_view k_futures_fills = "/api/v1/fills";
inline constexpr std::string_view k_positions = "/api/v1/positions";
// futures only, one settle currency per call
inline constexpr std::string_view k_account_overview = "/api/v1/account-overview";

// perpetuals and dated contracts; only type tells them apart
inline constexpr std::string_view k_contracts_active = "/api/v1/contracts/active";
// spot host
inline constexpr std::string_view k_spot_symbols = "/api/v2/symbols";
inline constexpr std::string_view k_spot_all_tickers = "/api/v1/market/allTickers";
// symbol sits mid-path: k_funding_rate + "/" + symbol + k_funding_rate_current
inline constexpr std::string_view k_funding_rate = "/api/v1/funding-rate";
inline constexpr std::string_view k_funding_rate_current = "/current";
inline constexpr std::string_view k_level2_snapshot = "/api/v1/level2/snapshot";
inline constexpr std::string_view k_server_time = "/api/v1/timestamp";

} // namespace raikiri::venues::kucoin::ep
