#pragma once

#include <cstdint>
#include <string_view>

namespace raikiri::venues::toobit::ep {

inline constexpr std::string_view k_rest_host = "api.toobit.com";
inline constexpr std::string_view k_ws_host = "stream.toobit.com";
inline constexpr std::string_view k_ws_path = "/quote/ws/v1";
inline constexpr std::string_view k_user_ws_prefix = "/api/v1/ws/";

inline constexpr std::string_view k_exchange_info = "/api/v1/exchangeInfo";
inline constexpr std::string_view k_funding_rate = "/api/v1/futures/fundingRate";
inline constexpr std::string_view k_index = "/quote/v1/index";
inline constexpr std::string_view k_contract_ticker = "/quote/v1/contract/ticker/24hr";
inline constexpr std::string_view k_spot_ticker = "/quote/v1/ticker/24hr";

inline constexpr std::string_view k_order = "/api/v1/futures/order";
inline constexpr std::string_view k_order_update = "/api/v1/futures/order/update";
inline constexpr std::string_view k_open_orders = "/api/v1/futures/openOrders";
inline constexpr std::string_view k_positions = "/api/v1/futures/positions";
inline constexpr std::string_view k_balance = "/api/v1/futures/balance";
inline constexpr std::string_view k_user_trades = "/api/v1/futures/userTrades";
inline constexpr std::string_view k_listen_key = "/api/v1/listenKey";

inline constexpr std::string_view k_api_key_header = "X-BB-APIKEY";
inline constexpr std::uint32_t k_recv_window_ms = 5000;

// the server closes a socket that has sent nothing for 300 s, data flowing or not.
inline constexpr std::uint64_t k_app_ping_interval_ns = 30'000'000'000ULL;
inline constexpr std::uint64_t k_funding_poll_ns = 15'000'000'000ULL;
inline constexpr std::uint64_t k_ticker_poll_ns = 60'000'000'000ULL;
inline constexpr std::uint64_t k_listen_key_interval_ns = 30ULL * 60 * 1'000'000'000ULL;

} // namespace raikiri::venues::toobit::ep
