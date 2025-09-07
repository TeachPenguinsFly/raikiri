#pragma once

// Every address this adapter speaks to, in one place.
//
// A path spelled inline is a path nobody can audit: when bybit moves one, or
// when a reader asks "what does this process actually call", the answer has to
// be greppable rather than scattered through a thousand lines of request
// building. It also keeps the testnet split honest — a host chosen per call
// site is a host that will one day be chosen wrongly.

#include <string_view>

namespace raikiri::venues::bybit::ep {

// --- hosts ---------------------------------------------------------------
inline constexpr std::string_view k_rest_live = "api.bybit.com";
inline constexpr std::string_view k_rest_test = "api-testnet.bybit.com";
inline constexpr std::string_view k_stream_live = "stream.bybit.com";
inline constexpr std::string_view k_stream_test = "stream-testnet.bybit.com";

[[nodiscard]] constexpr std::string_view rest_host(bool testnet) noexcept {
    return testnet ? k_rest_test : k_rest_live;
}

[[nodiscard]] constexpr std::string_view stream_host(bool testnet) noexcept {
    return testnet ? k_stream_test : k_stream_live;
}

// --- sockets -------------------------------------------------------------
inline constexpr std::string_view k_ws_public_linear = "/v5/public/linear";
inline constexpr std::string_view k_ws_public_spot = "/v5/public/spot";
inline constexpr std::string_view k_ws_private = "/v5/private";

// --- order entry ---------------------------------------------------------
inline constexpr std::string_view k_order_create = "/v5/order/create";
inline constexpr std::string_view k_order_cancel = "/v5/order/cancel";
inline constexpr std::string_view k_order_cancel_all = "/v5/order/cancel-all";
inline constexpr std::string_view k_order_amend = "/v5/order/amend";

// --- account and reconciliation -----------------------------------------
inline constexpr std::string_view k_order_open = "/v5/order/realtime";
inline constexpr std::string_view k_execution_list = "/v5/execution/list";
inline constexpr std::string_view k_position_list = "/v5/position/list";
inline constexpr std::string_view k_wallet_balance = "/v5/account/wallet-balance";
// Metered at 25 requests a second where the rest of v5 allows fifty; the rate
// budget in config/venues.yaml states that separately and must agree with it.
inline constexpr std::string_view k_transaction_log = "/v5/account/transaction-log";

// --- reference data ------------------------------------------------------
inline constexpr std::string_view k_instruments = "/v5/market/instruments-info";
inline constexpr std::string_view k_server_time = "/v5/market/time";

} // namespace raikiri::venues::bybit::ep
