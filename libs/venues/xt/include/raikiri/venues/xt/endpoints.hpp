#pragma once

// Every address this adapter speaks to, in one place.
//
// A path spelled inline is a path nobody can audit: when XT moves one, or when
// a reader asks "what does this process actually call", the answer has to be
// greppable rather than scattered through a thousand lines of request
// building.
//
// FOUR HOSTS. Spot and futures are two deployments with two APIs, and each has
// its own REST name and its own socket name; a host chosen per call site is a
// host that will one day be chosen wrongly.

#include <string_view>

namespace raikiri::venues::xt::ep {

// --- hosts ---------------------------------------------------------------
// XT publishes a demo host for spot only, and it does not serve fstream, so
// nothing here takes a testnet flag.
inline constexpr std::string_view k_rest_futures = "fapi.xt.com";
inline constexpr std::string_view k_rest_spot = "sapi.xt.com";
inline constexpr std::string_view k_stream_futures = "fstream.xt.com";
// Spot serves its public and its private socket from this one name.
inline constexpr std::string_view k_stream_spot = "stream.xt.com";

// --- sockets -------------------------------------------------------------
inline constexpr std::string_view k_ws_futures_market = "/ws/market";
inline constexpr std::string_view k_ws_futures_user = "/ws/user";
inline constexpr std::string_view k_ws_spot_public = "/public";
inline constexpr std::string_view k_ws_spot_private = "/private";

// --- order entry ---------------------------------------------------------
inline constexpr std::string_view k_futures_order_create = "/future/trade/v1/order/create";
inline constexpr std::string_view k_futures_order_cancel = "/future/trade/v1/order/cancel";
inline constexpr std::string_view k_futures_order_update = "/future/trade/v1/order/update";
// One path for four verbs: POST places, GET reads by client id, and DELETE and
// PUT take the venue order id as a trailing PATH SEGMENT — which puts it
// INSIDE the signature's path term rather than beside it.
inline constexpr std::string_view k_spot_order = "/v4/order";

// --- account and reconciliation -----------------------------------------
inline constexpr std::string_view k_futures_open_orders = "/future/trade/v1/order/list-open-order";
inline constexpr std::string_view k_futures_trades = "/future/trade/v1/order/trade-list";
inline constexpr std::string_view k_futures_positions = "/future/user/v1/position/list";
// fapi.xt.com is the U-BASED wallet; the coin-margined one is another host.
inline constexpr std::string_view k_futures_balances = "/future/user/v1/balance/list";
// The wallet's own ledger. `startTime` filters and `endTime` is REFUSED with
// returnCode 1, so the pull is a floor and never a window.
inline constexpr std::string_view k_futures_bills = "/future/user/v1/balance/bills";
inline constexpr std::string_view k_spot_open_orders = "/v4/open-order";
inline constexpr std::string_view k_spot_trades = "/v4/trade";
// The two socket credentials: futures mints a listen key, spot a ws token.
inline constexpr std::string_view k_futures_listen_key = "/future/user/v1/user/listen-key";
inline constexpr std::string_view k_spot_ws_token = "/v4/ws-token";

// --- reference data ------------------------------------------------------
inline constexpr std::string_view k_futures_symbols = "/future/market/v1/public/symbol/list";
inline constexpr std::string_view k_spot_symbols = "/v4/public/symbol";
// Two deployments, two clocks; neither stands in for the other.
inline constexpr std::string_view k_futures_server_time = "/future/market/v1/public/time";
inline constexpr std::string_view k_spot_server_time = "/v4/public/time";
inline constexpr std::string_view k_futures_funding_rate =
    "/future/market/v1/public/q/funding-rate";
inline constexpr std::string_view k_futures_depth = "/future/market/v1/public/q/depth";

} // namespace raikiri::venues::xt::ep
