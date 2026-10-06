#pragma once

#include <string_view>

namespace raikiri::venues::okx::ep {

// one rest host for live and demo, where demo is a header; the demo websocket is its own host
inline constexpr std::string_view k_rest = "www.okx.com";
inline constexpr std::string_view k_stream_live = "ws.okx.com";
inline constexpr std::string_view k_stream_demo = "wspap.okx.com";
// OKX serves its websockets on 8443, not on 443 like its REST.
inline constexpr std::string_view k_stream_port = "8443";

[[nodiscard]] constexpr std::string_view rest_host() noexcept {
    return k_rest;
}

[[nodiscard]] constexpr std::string_view stream_host(bool testnet) noexcept {
    return testnet ? k_stream_demo : k_stream_live;
}

inline constexpr std::string_view k_ws_public = "/ws/v5/public";
inline constexpr std::string_view k_ws_private = "/ws/v5/private";

inline constexpr std::string_view k_order_create = "/api/v5/trade/order";
inline constexpr std::string_view k_order_cancel = "/api/v5/trade/cancel-order";
inline constexpr std::string_view k_order_amend = "/api/v5/trade/amend-order";
inline constexpr std::string_view k_order_cancel_batch = "/api/v5/trade/cancel-batch-orders";

inline constexpr std::string_view k_orders_pending = "/api/v5/trade/orders-pending";
inline constexpr std::string_view k_fills = "/api/v5/trade/fills";
inline constexpr std::string_view k_positions = "/api/v5/account/positions";
// seven days deep; funding and fee rows are told apart by type
inline constexpr std::string_view k_bills = "/api/v5/account/bills";
// one call covers both markets: spot and swap draw on the same trading account
inline constexpr std::string_view k_balance = "/api/v5/account/balance";
// the account mode decides whether a per-currency available figure means anything
inline constexpr std::string_view k_account_config = "/api/v5/account/config";

inline constexpr std::string_view k_instruments = "/api/v5/public/instruments";
// options list by underlying: instruments refuses instType=OPTION without uly or instFamily
inline constexpr std::string_view k_underlyings = "/api/v5/public/underlying";
inline constexpr std::string_view k_mark_price = "/api/v5/public/mark-price";
inline constexpr std::string_view k_funding_rate = "/api/v5/public/funding-rate";
inline constexpr std::string_view k_books = "/api/v5/market/books";
inline constexpr std::string_view k_tickers = "/api/v5/market/tickers";
inline constexpr std::string_view k_open_interest = "/api/v5/public/open-interest";
inline constexpr std::string_view k_server_time = "/api/v5/public/time";

} // namespace raikiri::venues::okx::ep
