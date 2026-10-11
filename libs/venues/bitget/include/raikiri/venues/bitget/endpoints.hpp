#pragma once

// spot and mix share one host; the product type, demo included, is a query parameter

#include <string_view>

namespace raikiri::venues::bitget::ep {

inline constexpr std::string_view k_rest = "api.bitget.com";
inline constexpr std::string_view k_stream = "ws.bitget.com";

[[nodiscard]] constexpr std::string_view rest_host() noexcept {
    return k_rest;
}

[[nodiscard]] constexpr std::string_view stream_host() noexcept {
    return k_stream;
}

inline constexpr std::string_view k_ws_public = "/v2/ws/public";
inline constexpr std::string_view k_ws_private = "/v2/ws/private";

inline constexpr std::string_view k_spot_place_order = "/api/v2/spot/trade/place-order";
inline constexpr std::string_view k_spot_cancel_order = "/api/v2/spot/trade/cancel-order";
inline constexpr std::string_view k_spot_batch_cancel = "/api/v2/spot/trade/batch-cancel-order";
inline constexpr std::string_view k_mix_place_order = "/api/v2/mix/order/place-order";
inline constexpr std::string_view k_mix_cancel_order = "/api/v2/mix/order/cancel-order";
inline constexpr std::string_view k_mix_batch_cancel = "/api/v2/mix/order/batch-cancel-orders";

inline constexpr std::string_view k_spot_unfilled_orders = "/api/v2/spot/trade/unfilled-orders";
inline constexpr std::string_view k_spot_order_info = "/api/v2/spot/trade/orderInfo";
inline constexpr std::string_view k_spot_fills = "/api/v2/spot/trade/fills";
// lists pending orders only, so a finished mix order cannot be found here
inline constexpr std::string_view k_mix_orders_pending = "/api/v2/mix/order/orders-pending";
inline constexpr std::string_view k_mix_fills = "/api/v2/mix/order/fills";
inline constexpr std::string_view k_mix_positions = "/api/v2/mix/position/all-position";
inline constexpr std::string_view k_mix_account_bill = "/api/v2/mix/account/bill";

inline constexpr std::string_view k_mix_contracts = "/api/v2/mix/market/contracts";
inline constexpr std::string_view k_mix_ticker = "/api/v2/mix/market/ticker";
inline constexpr std::string_view k_mix_tickers = "/api/v2/mix/market/tickers";
inline constexpr std::string_view k_spot_tickers = "/api/v2/spot/market/tickers";
inline constexpr std::string_view k_mix_fund_rate = "/api/v2/mix/market/current-fund-rate";
inline constexpr std::string_view k_mix_orderbook = "/api/v2/mix/market/orderbook";
inline constexpr std::string_view k_spot_symbols = "/api/v2/spot/public/symbols";
inline constexpr std::string_view k_server_time = "/api/v2/public/time";

} // namespace raikiri::venues::bitget::ep
