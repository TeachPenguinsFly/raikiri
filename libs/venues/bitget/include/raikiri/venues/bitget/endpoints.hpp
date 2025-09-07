#pragma once

// Every address this adapter speaks to, in one place.
//
// A path spelled inline is a path nobody can audit: when Bitget moves one, or
// when a reader asks "what does this process actually call", the answer has to
// be greppable rather than scattered through a thousand lines of request
// building.
//
// Spot and futures ("mix") are two path families on one host, and every futures
// address takes the product type as a query parameter rather than in the path —
// demo trading is that parameter, not another hostname.

#include <string_view>

namespace raikiri::venues::bitget::ep {

// --- hosts ---------------------------------------------------------------
// One host each for live and demo alike.
inline constexpr std::string_view k_rest = "api.bitget.com";
inline constexpr std::string_view k_stream = "ws.bitget.com";

[[nodiscard]] constexpr std::string_view rest_host() noexcept {
    return k_rest;
}

[[nodiscard]] constexpr std::string_view stream_host() noexcept {
    return k_stream;
}

// --- sockets -------------------------------------------------------------
inline constexpr std::string_view k_ws_public = "/v2/ws/public";
inline constexpr std::string_view k_ws_private = "/v2/ws/private";

// --- order entry ---------------------------------------------------------
inline constexpr std::string_view k_spot_place_order = "/api/v2/spot/trade/place-order";
inline constexpr std::string_view k_spot_cancel_order = "/api/v2/spot/trade/cancel-order";
inline constexpr std::string_view k_spot_batch_cancel = "/api/v2/spot/trade/batch-cancel-order";
inline constexpr std::string_view k_mix_place_order = "/api/v2/mix/order/place-order";
inline constexpr std::string_view k_mix_cancel_order = "/api/v2/mix/order/cancel-order";
inline constexpr std::string_view k_mix_batch_cancel = "/api/v2/mix/order/batch-cancel-orders";

// --- account and reconciliation -----------------------------------------
inline constexpr std::string_view k_spot_unfilled_orders = "/api/v2/spot/trade/unfilled-orders";
inline constexpr std::string_view k_spot_order_info = "/api/v2/spot/trade/orderInfo";
inline constexpr std::string_view k_spot_fills = "/api/v2/spot/trade/fills";
// Lists only PENDING orders, and the read by client order id lands here too:
// a mix order that has already finished cannot be found through it.
inline constexpr std::string_view k_mix_orders_pending = "/api/v2/mix/order/orders-pending";
inline constexpr std::string_view k_mix_fills = "/api/v2/mix/order/fills";
inline constexpr std::string_view k_mix_positions = "/api/v2/mix/position/all-position";

// --- reference data ------------------------------------------------------
inline constexpr std::string_view k_mix_contracts = "/api/v2/mix/market/contracts";
inline constexpr std::string_view k_mix_ticker = "/api/v2/mix/market/ticker";
inline constexpr std::string_view k_mix_fund_rate = "/api/v2/mix/market/current-fund-rate";
inline constexpr std::string_view k_mix_orderbook = "/api/v2/mix/market/orderbook";
inline constexpr std::string_view k_spot_symbols = "/api/v2/spot/public/symbols";
inline constexpr std::string_view k_server_time = "/api/v2/public/time";

} // namespace raikiri::venues::bitget::ep
