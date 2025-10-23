#pragma once

// Every address this adapter speaks to, in one place.
//
// A path spelled inline is a path nobody can audit: when BingX moves one, or
// when a reader asks "what does this process actually call", the answer has to
// be greppable rather than scattered through a thousand lines of request
// building.
//
// One REST name serves both markets and the paths tell them apart
// (/openApi/swap vs /openApi/spot); the sockets do the opposite and split by
// host. A host chosen per call site is a host that will one day be chosen
// wrongly.

#include <string_view>

namespace raikiri::venues::bingx::ep {

// --- hosts ---------------------------------------------------------------
// BingX publishes no testnet: the config's flag has no host to select.
inline constexpr std::string_view k_rest = "open-api.bingx.com";
inline constexpr std::string_view k_stream_perp = "open-api-swap.bingx.com";
inline constexpr std::string_view k_stream_spot = "open-api-ws.bingx.com";

// --- sockets -------------------------------------------------------------
// Each market puts its user stream on the SAME host and path as its public
// one; the listen key in the query is the only thing that separates them.
inline constexpr std::string_view k_ws_perp = "/swap-market";
inline constexpr std::string_view k_ws_spot = "/market";

// --- order entry ---------------------------------------------------------
// POST places and DELETE cancels on the perpetual tree.
inline constexpr std::string_view k_perp_order = "/openApi/swap/v2/trade/order";
inline constexpr std::string_view k_perp_amend = "/openApi/swap/v1/trade/amend";
inline constexpr std::string_view k_spot_order = "/openApi/spot/v1/trade/order";
// Spot cancels through a path of its own, and with a POST rather than a
// DELETE — one segment away from the perpetual tree, nothing alike.
inline constexpr std::string_view k_spot_cancel = "/openApi/spot/v1/trade/cancel";

// --- account and reconciliation -----------------------------------------
inline constexpr std::string_view k_perp_open_orders = "/openApi/swap/v2/trade/openOrders";
inline constexpr std::string_view k_spot_open_orders = "/openApi/spot/v1/trade/openOrders";
inline constexpr std::string_view k_perp_positions = "/openApi/swap/v2/user/positions";
// The margin wallet, and the account's own book three months deep. The two
// sit on DIFFERENT VERSIONS of the same tree and that is BingX's spelling, not
// a typo: v2/user/balance still answers and states one asset under `data
// .balance`, where v3 states every settlement asset as an array.
inline constexpr std::string_view k_perp_balance = "/openApi/swap/v3/user/balance";
inline constexpr std::string_view k_perp_income = "/openApi/swap/v2/user/income";
inline constexpr std::string_view k_position_mode = "/openApi/swap/v1/positionSide/dual";
// POST opens, PUT keeps alive, DELETE closes — and it serves both markets.
inline constexpr std::string_view k_listen_key = "/openApi/user/auth/userDataStream";

// --- reference data ------------------------------------------------------
inline constexpr std::string_view k_perp_contracts = "/openApi/swap/v2/quote/contracts";
inline constexpr std::string_view k_spot_symbols = "/openApi/spot/v1/common/symbols";
inline constexpr std::string_view k_server_time = "/openApi/swap/v2/server/time";
inline constexpr std::string_view k_premium_index = "/openApi/swap/v2/quote/premiumIndex";
inline constexpr std::string_view k_depth = "/openApi/swap/v2/quote/depth";
inline constexpr std::string_view k_ticker = "/openApi/swap/v2/quote/ticker";

} // namespace raikiri::venues::bingx::ep
