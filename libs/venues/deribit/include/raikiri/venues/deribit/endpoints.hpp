#pragma once

#include <cstddef>
#include <string_view>

namespace raikiri::venues::deribit::ep {

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

inline constexpr std::string_view k_ws = "/ws/api/v2";

inline constexpr std::string_view k_order_buy = "/api/v2/private/buy";
inline constexpr std::string_view k_order_sell = "/api/v2/private/sell";
inline constexpr std::string_view k_order_cancel = "/api/v2/private/cancel";
// unscoped it sweeps every currency; the call site pins one
inline constexpr std::string_view k_order_cancel_by_label = "/api/v2/private/cancel_by_label";
inline constexpr std::string_view k_order_edit = "/api/v2/private/edit";
// works only while exactly one open order carries the label
inline constexpr std::string_view k_order_edit_by_label = "/api/v2/private/edit_by_label";

inline constexpr std::string_view k_open_orders_by_currency =
    "/api/v2/private/get_open_orders_by_currency";
inline constexpr std::string_view k_order_state_by_label =
    "/api/v2/private/get_order_state_by_label";
inline constexpr std::string_view k_user_trades_by_currency =
    "/api/v2/private/get_user_trades_by_currency";
inline constexpr std::string_view k_positions = "/api/v2/private/get_positions";

inline constexpr std::string_view k_instruments = "/api/v2/public/get_instruments";
inline constexpr std::string_view k_server_time = "/api/v2/public/get_time";

// one request bucket per ip shared by every socket: burst 100, refill 20/s
inline constexpr std::size_t k_ip_request_burst = 100;
inline constexpr std::size_t k_ip_requests_per_s = 20;
// larger frames are closed with 1009
inline constexpr std::size_t k_max_frame_bytes = 32768;

} // namespace raikiri::venues::deribit::ep
