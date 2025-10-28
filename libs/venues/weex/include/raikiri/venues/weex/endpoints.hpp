#pragma once

// Every address this adapter speaks to, in one place.
//
// A path spelled inline is a path nobody can audit: when WEEX moves one, or
// when a reader asks "what does this process actually call", the answer has to
// be greppable rather than scattered through a thousand lines of request
// building.
//
// WEEX runs its spot exchange on OTHER HOSTS with another path prefix, and
// nothing here addresses it: api-spot.weex.com serves /api/v3/... and
// api-contract.weex.com serves /capi/v3/..., and neither answers the other's
// paths — `GET api-contract.weex.com/api/v3/ping` and `GET
// api-spot.weex.com/capi/v3/market/time` were both 404 on 2026-09-13. It is
// one exchange behind one API key, not two, but it is two hosts and this
// adapter reaches one of them.
//
// There is no testnet: the documented hosts below are the only ones WEEX
// publishes, so `--testnet` has nowhere else to point. The demo account rides
// /capi/v3/sim/... on this same host and is not wired here.

#include <string_view>

namespace raikiri::venues::weex::ep {

// --- hosts ---------------------------------------------------------------
inline constexpr std::string_view k_rest_host = "api-contract.weex.com";
inline constexpr std::string_view k_stream_host = "ws-contract.weex.com";
inline constexpr std::string_view k_stream_port = "443";

// --- sockets -------------------------------------------------------------
// One public socket for every channel. WEEX does publish a private socket at
// /v3/ws/private — authenticated by ACCESS-* headers on the HANDSHAKE, with no
// listen key anywhere — and this adapter does not open it: an order here is
// answered by its own REST response, and a push path nobody has ever received
// a frame from would be four parsers nothing exercises.
inline constexpr std::string_view k_ws_public = "/v3/ws/public";

// --- reference data ------------------------------------------------------
// `assets`, `rateLimits` and `symbols` in one body; 1022 contracts and 685 KiB
// measured 2026-09-07, which is why it is read once at start and never polled.
inline constexpr std::string_view k_exchange_info = "/capi/v3/market/exchangeInfo";

// Funding for every contract in one answer: lastFundingRate, the forecast for
// the next interval, nextFundingTime and collectCycle. WEEX pushes none of
// this on any socket, so it is polled or it does not arrive at all.
inline constexpr std::string_view k_premium_index = "/capi/v3/market/premiumIndex";

// {"serverTime":<ms>}. Read once at start by the SIGNED half: WEEX refuses a
// signature stamped more than 30 s from its own clock, and a laptop that has
// drifted wears the same face as a revoked key.
inline constexpr std::string_view k_server_time = "/capi/v3/market/time";

// WHICH CONTRACTS ORDERS MAY BE SENT FOR, which is not the same set as the
// catalogue. A bare JSON array of symbol names; 290 of the 1000 rows
// exchangeInfo lists, measured 2026-09-15. Everything else answers -1058 to
// any order — including every one of the five contracts WEEX funds hourly —
// while still streaming a perfectly good book, which is why the catalogue
// keeps them and only order entry consults this.
inline constexpr std::string_view k_api_trading_symbols = "/capi/v3/market/apiTradingSymbols";

// --- private -------------------------------------------------------------
//
// NOT ONE OF THESE HAS BEEN ANSWERED WITH DATA. There are no WEEX credentials
// on this machine, so every path below has been exercised only as far as the
// venue's own refusal: each answered HTTP 401 {"code":-1044,"msg":"Invalid
// ACCESS_KEY."} unauthenticated on 2026-09-13, which proves the path ROUTES
// and nothing whatever about the shape of a successful body. A path WEEX does
// not route answers {"timestamp","status":404,...} instead, which is how the
// two were told apart.

// POST places one order and DELETE cancels one. WEEX reads a third verb here
// — GET, by `orderId` only — and that one is absent: this process does not know
// the venue's id for an order it has lost, which is the only case a single-order
// read would serve, so query_order goes through the open-order list instead.
inline constexpr std::string_view k_order = "/capi/v3/order";

// DELETE cancels up to ten at once, by `orderIdList` or `origClientOrderIdList`.
// Its POST places up to five and is absent for the reason above: nothing in this
// process batches a create.
inline constexpr std::string_view k_batch_orders = "/capi/v3/batchOrders";

// What the venue still has WORKING, paged: `limit` maxes at 100 and `page`
// counts from zero.
inline constexpr std::string_view k_open_orders = "/capi/v3/openOrders";

// Fills. Windowed: a span wider than 7 days is refused, and with no window at
// all WEEX answers the last 7 days.
inline constexpr std::string_view k_user_trades = "/capi/v3/userTrades";

// Every open position in one answer, no parameters at all.
inline constexpr std::string_view k_all_positions = "/capi/v3/account/position/allPosition";

// What the account holds, coin by coin. No parameters.
inline constexpr std::string_view k_balance = "/capi/v3/account/balance";

// The account's own ledger, and a POST WITH A BODY rather than a GET —
// alone among the reads here. Funding is the `position_funding` rows of it.
inline constexpr std::string_view k_income = "/capi/v3/account/income";

} // namespace raikiri::venues::weex::ep
