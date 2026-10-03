#pragma once

#include <string_view>

namespace raikiri::venues::bybit::ep {

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

inline constexpr std::string_view k_ws_public_linear = "/v5/public/linear";
inline constexpr std::string_view k_ws_public_spot = "/v5/public/spot";
inline constexpr std::string_view k_ws_private = "/v5/private";
inline constexpr std::string_view k_ws_trade = "/v5/trade";

inline constexpr std::string_view k_order_create = "/v5/order/create";
inline constexpr std::string_view k_order_cancel = "/v5/order/cancel";
inline constexpr std::string_view k_order_cancel_all = "/v5/order/cancel-all";
inline constexpr std::string_view k_order_amend = "/v5/order/amend";

inline constexpr std::string_view k_order_open = "/v5/order/realtime";
inline constexpr std::string_view k_execution_list = "/v5/execution/list";
inline constexpr std::string_view k_position_list = "/v5/position/list";
inline constexpr std::string_view k_wallet_balance = "/v5/account/wallet-balance";
// metered at 25 requests a second; config/venues.yaml must agree
inline constexpr std::string_view k_transaction_log = "/v5/account/transaction-log";

inline constexpr std::string_view k_instruments = "/v5/market/instruments-info";
inline constexpr std::string_view k_server_time = "/v5/market/time";

} // namespace raikiri::venues::bybit::ep
