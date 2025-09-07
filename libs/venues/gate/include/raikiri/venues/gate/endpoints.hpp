#pragma once

// Every address this adapter speaks to, in one place.
//
// A path spelled inline is a path nobody can audit: when Gate moves one, or
// when a reader asks "what does this process actually call", the answer has to
// be greppable rather than scattered through a thousand lines of request
// building. It also keeps the testnet split honest — a host chosen per call
// site is a host that will one day be chosen wrongly, and Gate offers five
// different hostnames to choose wrongly between.
//
// Futures addresses carry the settlement currency mid-path, so they are spelled
// here as a prefix and a tail: k_futures_prefix + settle + k_futures_orders is
// /api/v4/futures/usdt/orders. Spot paths have no such hole and are whole.

#include <string_view>

namespace raikiri::venues::gate::ep {

// --- hosts ---------------------------------------------------------------
inline constexpr std::string_view k_rest_live = "api.gateio.ws";
inline constexpr std::string_view k_rest_test = "api-testnet.gateapi.io";
inline constexpr std::string_view k_futures_stream_live = "fx-ws.gateio.ws";
inline constexpr std::string_view k_futures_stream_test = "fx-ws-testnet.gateio.ws";
// Spot streams from the REST host, and its testnet is a fifth name again.
inline constexpr std::string_view k_spot_stream_live = "api.gateio.ws";
inline constexpr std::string_view k_spot_stream_test = "ws-testnet.gate.com";

[[nodiscard]] constexpr std::string_view rest_host(bool testnet) noexcept {
    return testnet ? k_rest_test : k_rest_live;
}

[[nodiscard]] constexpr std::string_view futures_stream_host(bool testnet) noexcept {
    return testnet ? k_futures_stream_test : k_futures_stream_live;
}

[[nodiscard]] constexpr std::string_view spot_stream_host(bool testnet) noexcept {
    return testnet ? k_spot_stream_test : k_spot_stream_live;
}

// --- sockets -------------------------------------------------------------
inline constexpr std::string_view k_ws_futures_usdt = "/v4/ws/usdt";
// The live spot socket keeps its trailing slash; the testnet one is a
// different path entirely rather than the same path on another host.
inline constexpr std::string_view k_ws_spot_live = "/ws/v4/";
inline constexpr std::string_view k_ws_spot_test = "/v4/ws/spot";

[[nodiscard]] constexpr std::string_view ws_spot_path(bool testnet) noexcept {
    return testnet ? k_ws_spot_test : k_ws_spot_live;
}

// --- futures path prefix -------------------------------------------------
// The settlement currency goes here, then one of the tails below.
inline constexpr std::string_view k_futures_prefix = "/api/v4/futures/";

// --- order entry ---------------------------------------------------------
// One tail for four verbs; an order id, when there is one, is appended to it.
inline constexpr std::string_view k_futures_orders = "/orders";
inline constexpr std::string_view k_futures_batch_cancel = "/batch_cancel_orders";
inline constexpr std::string_view k_spot_orders = "/api/v4/spot/orders";
inline constexpr std::string_view k_spot_cancel_batch = "/api/v4/spot/cancel_batch_orders";

// --- account and reconciliation -----------------------------------------
// The plain /my_trades pages by order id and cannot answer "since"; only the
// _timerange variant takes from/to, and spot has no such variant to need.
inline constexpr std::string_view k_futures_my_trades = "/my_trades_timerange";
inline constexpr std::string_view k_futures_positions = "/positions";
inline constexpr std::string_view k_futures_account_book = "/account_book";
inline constexpr std::string_view k_futures_accounts = "/accounts";
// Answers one entry PER CURRENCY PAIR, each holding its own `orders` array.
inline constexpr std::string_view k_spot_open_orders = "/api/v4/spot/open_orders";
inline constexpr std::string_view k_spot_my_trades = "/api/v4/spot/my_trades";

// --- reference data ------------------------------------------------------
inline constexpr std::string_view k_futures_contracts = "/contracts";
inline constexpr std::string_view k_futures_order_book = "/order_book";
inline constexpr std::string_view k_spot_currency_pairs = "/api/v4/spot/currency_pairs";
inline constexpr std::string_view k_server_time = "/api/v4/spot/time";

} // namespace raikiri::venues::gate::ep
