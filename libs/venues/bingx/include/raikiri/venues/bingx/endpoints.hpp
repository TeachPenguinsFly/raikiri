#pragma once

#include <string_view>

namespace raikiri::venues::bingx::ep {

// bingx has no testnet
inline constexpr std::string_view k_rest = "open-api.bingx.com";
inline constexpr std::string_view k_stream_perp = "open-api-swap.bingx.com";
inline constexpr std::string_view k_stream_spot = "open-api-ws.bingx.com";

// the user stream shares the public host and path; the listen key tells them apart
inline constexpr std::string_view k_ws_perp = "/swap-market";
inline constexpr std::string_view k_ws_spot = "/market";

// post places and delete cancels
inline constexpr std::string_view k_perp_order = "/openApi/swap/v2/trade/order";
inline constexpr std::string_view k_perp_amend = "/openApi/swap/v1/trade/amend";
inline constexpr std::string_view k_spot_order = "/openApi/spot/v1/trade/order";
// spot cancels with a post on its own path
inline constexpr std::string_view k_spot_cancel = "/openApi/spot/v1/trade/cancel";

inline constexpr std::string_view k_perp_open_orders = "/openApi/swap/v2/trade/openOrders";
inline constexpr std::string_view k_spot_open_orders = "/openApi/spot/v1/trade/openOrders";
inline constexpr std::string_view k_perp_positions = "/openApi/swap/v2/user/positions";
// v3 on purpose: v2/user/balance returns a single asset
inline constexpr std::string_view k_perp_balance = "/openApi/swap/v3/user/balance";
inline constexpr std::string_view k_perp_income = "/openApi/swap/v2/user/income";
inline constexpr std::string_view k_position_mode = "/openApi/swap/v1/positionSide/dual";
// post opens, put keeps alive, delete closes; serves both markets
inline constexpr std::string_view k_listen_key = "/openApi/user/auth/userDataStream";

inline constexpr std::string_view k_perp_contracts = "/openApi/swap/v2/quote/contracts";
inline constexpr std::string_view k_spot_symbols = "/openApi/spot/v1/common/symbols";
inline constexpr std::string_view k_server_time = "/openApi/swap/v2/server/time";
inline constexpr std::string_view k_premium_index = "/openApi/swap/v2/quote/premiumIndex";
inline constexpr std::string_view k_depth = "/openApi/swap/v2/quote/depth";
inline constexpr std::string_view k_ticker = "/openApi/swap/v2/quote/ticker";

} // namespace raikiri::venues::bingx::ep
