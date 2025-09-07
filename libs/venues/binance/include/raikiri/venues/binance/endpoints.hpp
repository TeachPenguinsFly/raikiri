#pragma once

// Every address this adapter speaks to, in one place.
//
// A path spelled inline is a path nobody can audit: when binance moves one, or
// when a reader asks "what does this process actually call", the answer has to
// be greppable rather than scattered through a thousand lines of request
// building. It also keeps the testnet split honest — a host chosen per call
// site is a host that will one day be chosen wrongly.
//
// Everything here is the USD-margined futures API (fapi). Binance spot is
// another host behind another API and this adapter never reaches it.

#include <string_view>

namespace raikiri::venues::binance::ep {

// --- hosts ---------------------------------------------------------------
inline constexpr std::string_view k_rest_live = "fapi.binance.com";
inline constexpr std::string_view k_rest_test = "testnet.binancefuture.com";
inline constexpr std::string_view k_stream_live = "fstream.binance.com";
inline constexpr std::string_view k_stream_test = "fstream.binancefuture.com";

[[nodiscard]] constexpr std::string_view rest_host(bool testnet) noexcept {
    return testnet ? k_rest_test : k_rest_live;
}

[[nodiscard]] constexpr std::string_view stream_host(bool testnet) noexcept {
    return testnet ? k_stream_test : k_stream_live;
}

// --- sockets -------------------------------------------------------------
inline constexpr std::string_view k_ws_combined = "/stream";
// The user data socket addresses itself with the listen key: this prefix is
// only half a target and the key is appended at the call site.
inline constexpr std::string_view k_ws_user_prefix = "/ws/";

// --- order entry ---------------------------------------------------------
// One path for four verbs: POST places, DELETE cancels, PUT amends, GET reads.
inline constexpr std::string_view k_order = "/fapi/v1/order";

// --- account and reconciliation -----------------------------------------
inline constexpr std::string_view k_open_orders = "/fapi/v1/openOrders";
inline constexpr std::string_view k_income = "/fapi/v1/income";
inline constexpr std::string_view k_user_trades = "/fapi/v1/userTrades";
inline constexpr std::string_view k_position_risk = "/fapi/v3/positionRisk";
// Asked at v2 while positionRisk above is asked at v3: binance versions these
// endpoints one at a time, and the mismatch is the current shape of the API
// rather than a stale path.
inline constexpr std::string_view k_balance = "/fapi/v2/balance";
// POST opens, PUT keeps alive, DELETE closes.
inline constexpr std::string_view k_listen_key = "/fapi/v1/listenKey";

// --- reference data ------------------------------------------------------
inline constexpr std::string_view k_exchange_info = "/fapi/v1/exchangeInfo";
inline constexpr std::string_view k_premium_index = "/fapi/v1/premiumIndex";
inline constexpr std::string_view k_depth = "/fapi/v1/depth";
inline constexpr std::string_view k_server_time = "/fapi/v1/time";

} // namespace raikiri::venues::binance::ep
