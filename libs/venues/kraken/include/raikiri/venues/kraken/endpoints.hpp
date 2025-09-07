#pragma once

// Every address this adapter speaks to, in one place.
//
// A path spelled inline is a path nobody can audit: when Kraken moves one, or
// when a reader asks "what does this process actually call", the answer has to
// be greppable rather than scattered through a thousand lines of request
// building.
//
// THE PATHS BELOW ARE THE SIGNED FORM. Kraken Futures sends
// /derivatives/api/v3/sendorder and signs /api/v3/sendorder, so k_route_prefix
// is prepended when the target is written and never when the digest is built.
// Spelling a path with the prefix already in it is what makes a correct
// request authenticate as a bad key.

#include <string_view>

namespace raikiri::venues::kraken::ep {

// --- hosts ---------------------------------------------------------------
// One host for REST and the socket alike. VERIFIED: demo-futures.kraken.com
// answers 301, so the config's testnet flag has no host to select.
inline constexpr std::string_view k_rest = "futures.kraken.com";
inline constexpr std::string_view k_stream = "futures.kraken.com";
// The prefix every REST target carries and no signature does.
inline constexpr std::string_view k_route_prefix = "/derivatives";

// --- sockets -------------------------------------------------------------
// The private feeds are subscribed on this same socket after a challenge; the
// route prefix is a REST fact and has no place in this target.
inline constexpr std::string_view k_ws = "/ws/v1";

// --- order entry ---------------------------------------------------------
inline constexpr std::string_view k_send_order = "/api/v3/sendorder";
inline constexpr std::string_view k_cancel_order = "/api/v3/cancelorder";
inline constexpr std::string_view k_edit_order = "/api/v3/editorder";

// --- account and reconciliation -----------------------------------------
inline constexpr std::string_view k_open_orders = "/api/v3/openorders";
inline constexpr std::string_view k_fills = "/api/v3/fills";
inline constexpr std::string_view k_open_positions = "/api/v3/openpositions";

// --- reference data ------------------------------------------------------
// There is no clock endpoint: every response carries `serverTime`, and the
// ticker below is the cheapest such read. The symbol is a trailing PATH
// SEGMENT on it and a query parameter on the orderbook.
inline constexpr std::string_view k_instruments = "/api/v3/instruments";
inline constexpr std::string_view k_tickers = "/api/v3/tickers";
inline constexpr std::string_view k_orderbook = "/api/v3/orderbook";

} // namespace raikiri::venues::kraken::ep
