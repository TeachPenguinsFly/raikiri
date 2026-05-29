#pragma once

// Every address this adapter speaks to, in one place.
//
// A path spelled inline is a path nobody can audit: when MEXC moves one, or
// when a reader asks "what does this process actually call", the answer has to
// be greppable rather than scattered through a thousand lines of request
// building.
//
// THREE DEPLOYMENTS WEARING ONE BRAND. Public futures market data, every
// signed call, and the spot socket each answer from a different name, and the
// /api/v1 (futures) and /api/v3 (spot) trees sign nothing alike — see
// rest.hpp. A host chosen per call site is a host that will one day be chosen
// wrongly.

#include <string_view>

namespace raikiri::venues::mexc::ep {

// --- hosts ---------------------------------------------------------------
// MEXC runs no testnet on either tree, so nothing here takes a flag.
inline constexpr std::string_view k_rest_public = "contract.mexc.com";
// Every SIGNED call, spot and futures alike. VERIFIED 2026-08-26: the public
// host 403s a private POST at its CDN.
inline constexpr std::string_view k_rest_private = "api.mexc.com";
inline constexpr std::string_view k_stream_futures = "contract.mexc.com";
inline constexpr std::string_view k_stream_spot = "wbs-api.mexc.com";

// --- sockets -------------------------------------------------------------
// The futures socket carries the private stream too: the login rides the same
// connection the public channels use, and there is no separate endpoint.
inline constexpr std::string_view k_ws_futures = "/edge";
inline constexpr std::string_view k_ws_spot = "/ws";

// --- order entry ---------------------------------------------------------
inline constexpr std::string_view k_futures_order_create = "/api/v1/private/order/create";
// Takes an ARRAY of venue ids and cannot express a client id; the one below is
// the by-client-id form and cancels exactly one order.
inline constexpr std::string_view k_futures_order_cancel = "/api/v1/private/order/cancel";
inline constexpr std::string_view k_futures_order_cancel_by_client_id =
    "/api/v1/private/order/cancel_with_external";
// One path for three verbs: POST places, DELETE cancels, GET reads.
inline constexpr std::string_view k_spot_order = "/api/v3/order";

// --- account and reconciliation -----------------------------------------
// An optional symbol follows as a trailing PATH SEGMENT, appended at the call
// site; a trailing empty one is a different endpoint generation.
inline constexpr std::string_view k_futures_open_orders = "/api/v1/private/order/list/open_orders";
// Takes /{symbol}/{externalOid} after it.
inline constexpr std::string_view k_futures_order_by_client_id = "/api/v1/private/order/external";
inline constexpr std::string_view k_futures_order_deals = "/api/v1/private/order/list/order_deals";
inline constexpr std::string_view k_futures_positions = "/api/v1/private/position/open_positions";
// The account's own funding ledger. `symbol` is OPTIONAL on this one, which is
// what lets a pull that names only a market ask it at all.
inline constexpr std::string_view k_futures_funding_records =
    "/api/v1/private/position/funding_records";
inline constexpr std::string_view k_spot_open_orders = "/api/v3/openOrders";
inline constexpr std::string_view k_spot_my_trades = "/api/v3/myTrades";

// --- reference data ------------------------------------------------------
inline constexpr std::string_view k_contract_detail = "/api/v1/contract/detail";
inline constexpr std::string_view k_exchange_info = "/api/v3/exchangeInfo";
// Two clocks, not one: spot signs against ITS deployment's time, so neither of
// these stands in for the other.
inline constexpr std::string_view k_futures_server_time = "/api/v1/contract/ping";
inline constexpr std::string_view k_spot_server_time = "/api/v3/time";
inline constexpr std::string_view k_spot_ticker_24h = "/api/v3/ticker/24hr";
// The symbol is a trailing PATH SEGMENT on these two, a query parameter on the
// ticker below.
inline constexpr std::string_view k_contract_depth = "/api/v1/contract/depth";
inline constexpr std::string_view k_contract_funding_rate = "/api/v1/contract/funding_rate";
inline constexpr std::string_view k_contract_ticker = "/api/v1/contract/ticker";

} // namespace raikiri::venues::mexc::ep
