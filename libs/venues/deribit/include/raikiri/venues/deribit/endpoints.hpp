#pragma once

// Every address this adapter speaks to, in one place.
//
// A path spelled inline is a path nobody can audit: when Deribit moves one, or
// when a reader asks "what does this process actually call", the answer has to
// be greppable rather than scattered through a thousand lines of request
// building. It also keeps the testnet split honest — a host chosen per call
// site is a host that will one day be chosen wrongly.
//
// Deribit is one JSON-RPC API served two ways: over the socket the name below
// the /api/v2 prefix is the RPC `method`, and over REST the same name IS the
// path. These are the REST spellings; every private one is a GET whose
// parameters ride in the query.

#include <string_view>

namespace raikiri::venues::deribit::ep {

// --- hosts ---------------------------------------------------------------
inline constexpr std::string_view k_rest_live = "www.deribit.com";
inline constexpr std::string_view k_rest_test = "test.deribit.com";
inline constexpr std::string_view k_stream_live = "www.deribit.com";
inline constexpr std::string_view k_stream_test = "test.deribit.com";

[[nodiscard]] constexpr std::string_view rest_host(bool testnet) noexcept {
    return testnet ? k_rest_test : k_rest_live;
}

[[nodiscard]] constexpr std::string_view stream_host(bool testnet) noexcept {
    return testnet ? k_stream_test : k_stream_live;
}

// --- sockets -------------------------------------------------------------
inline constexpr std::string_view k_ws = "/ws/api/v2";

// --- order entry ---------------------------------------------------------
// The side is the endpoint here, not a parameter.
inline constexpr std::string_view k_order_buy = "/api/v2/private/buy";
inline constexpr std::string_view k_order_sell = "/api/v2/private/sell";
inline constexpr std::string_view k_order_cancel = "/api/v2/private/cancel";
// Unscoped, this one sweeps every currency; the call site pins it to one.
inline constexpr std::string_view k_order_cancel_by_label = "/api/v2/private/cancel_by_label";
inline constexpr std::string_view k_order_edit = "/api/v2/private/edit";
// Works only while exactly ONE open order carries the label.
inline constexpr std::string_view k_order_edit_by_label = "/api/v2/private/edit_by_label";

// --- account and reconciliation -----------------------------------------
inline constexpr std::string_view k_open_orders_by_currency =
    "/api/v2/private/get_open_orders_by_currency";
inline constexpr std::string_view k_order_state_by_label =
    "/api/v2/private/get_order_state_by_label";
inline constexpr std::string_view k_user_trades_by_currency =
    "/api/v2/private/get_user_trades_by_currency";
inline constexpr std::string_view k_positions = "/api/v2/private/get_positions";

// --- reference data ------------------------------------------------------
inline constexpr std::string_view k_instruments = "/api/v2/public/get_instruments";
inline constexpr std::string_view k_server_time = "/api/v2/public/get_time";

} // namespace raikiri::venues::deribit::ep
