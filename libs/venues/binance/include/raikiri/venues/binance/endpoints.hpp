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

#include <cstdint>
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
//
// "/private/ws/", NOT "/ws/". Binance decommissioned the legacy user-data path
// on 2026-04-23, and it fails in the worst way available: "/ws/<listenKey>"
// still completes the websocket handshake, still answers the server's pings,
// and stays open for hours delivering ZERO account events. Nothing about the
// connection looks wrong — connectivity reports Connected, the listen key is
// valid, keepalive succeeds — while every fill goes unreported, the position
// book drifts from the venue, and reconciliation eventually halts the kernel
// far away from the cause.
//
// MEASURED 2026-09-15, both paths, same key, same order: "/ws/" 101 then
// silence; "/private/ws/" 101 then ORDER_TRADE_UPDATE in the same second.
inline constexpr std::string_view k_ws_user_prefix = "/private/ws/";

// --- order entry ---------------------------------------------------------
// One path for four verbs: POST places, DELETE cancels, PUT amends, GET reads.
inline constexpr std::string_view k_order = "/fapi/v1/order";

// --- account and reconciliation -----------------------------------------
inline constexpr std::string_view k_open_orders = "/fapi/v1/openOrders";
inline constexpr std::string_view k_income = "/fapi/v1/income";
inline constexpr std::string_view k_user_trades = "/fapi/v1/userTrades";
inline constexpr std::string_view k_position_risk = "/fapi/v3/positionRisk";
// THE WALLET IS READ HERE AND NOT FROM /fapi/v2/balance, because this is the
// only answer that also states `multiAssetsMargin` — and without the mode, the
// per-asset `availableBalance` on either endpoint cannot be published at all:
// in multi-assets mode binance repeats ONE account-wide figure on every row.
//
// v3 of this path exists and is the newer revision of the same read, and it
// DROPPED multiAssetsMargin; /fapi/v1/accountConfig still states it, but as a
// second call. The mode and the figures it qualifies have to come out of one
// snapshot — two reads can straddle a mode switch, and the row that lands on
// the wrong side of it claims a whole account's margin as free BTC.
//
// Asked at v2 while positionRisk above is asked at v3: binance versions these
// endpoints one at a time, and the mismatch is the current shape of the API
// rather than a stale path.
inline constexpr std::string_view k_account = "/fapi/v2/account";
// POST opens, PUT keeps alive, DELETE closes.
inline constexpr std::string_view k_listen_key = "/fapi/v1/listenKey";

// --- reference data ------------------------------------------------------
inline constexpr std::string_view k_exchange_info = "/fapi/v1/exchangeInfo";
inline constexpr std::string_view k_premium_index = "/fapi/v1/premiumIndex";
// Every symbol in one answer. Weight 40 for the whole table against 1 per
// symbol, which is why nothing here asks per symbol.
inline constexpr std::string_view k_ticker_24h = "/fapi/v1/ticker/24hr";
inline constexpr std::string_view k_depth = "/fapi/v1/depth";
inline constexpr std::string_view k_server_time = "/fapi/v1/time";
// THE SETTLEMENT SCHEDULE, AND THE ONLY PLACE BINANCE STATES IT. exchangeInfo
// carries every other trading rule for a perp but not how often it funds, and
// the catalogue answered eight hours for all of them — while binance settles
// 465 of its 571 perps every four hours and five of them every hour. This
// endpoint answers all 571 in one call.
inline constexpr std::string_view k_funding_info = "/fapi/v1/fundingInfo";

} // namespace raikiri::venues::binance::ep
