#pragma once

// the host is part of the signature, so a request sent to the other host fails as a bad key

#include <string_view>

namespace raikiri::venues::htx::ep {

// htx has no testnet on either product; these are the hosts htx serves inside aws
inline constexpr std::string_view k_rest_futures = "api.hbdm.vn";
inline constexpr std::string_view k_rest_spot = "api-aws.huobi.pro";
inline constexpr std::string_view k_stream_futures = "api.hbdm.vn";
inline constexpr std::string_view k_stream_spot = "api-aws.huobi.pro";

// funding_rate is served only on the notification socket
inline constexpr std::string_view k_ws_market = "/linear-swap-ws";
inline constexpr std::string_view k_ws_notify = "/linear-swap-notification";
inline constexpr std::string_view k_ws_spot_private = "/ws/v2";

// cross-margin family; isolated drops the _cross infix and needs rest.hpp and ws.hpp changed with it
inline constexpr std::string_view k_swap_order = "/linear-swap-api/v1/swap_cross_order";
// one path for single and batch cancel
inline constexpr std::string_view k_swap_cancel = "/linear-swap-api/v1/swap_cross_cancel";
inline constexpr std::string_view k_spot_order_place = "/v1/order/orders/place";
// the venue order id is an infix: /v1/order/orders/{id}/submitcancel
inline constexpr std::string_view k_spot_orders = "/v1/order/orders";
inline constexpr std::string_view k_spot_submit_cancel_suffix = "/submitcancel";
inline constexpr std::string_view k_spot_cancel_by_client_id = "/v1/order/orders/submitCancelClientOrder";
inline constexpr std::string_view k_spot_batch_cancel = "/v1/order/orders/batchcancel";

inline constexpr std::string_view k_swap_open_orders = "/linear-swap-api/v1/swap_cross_openorders";
inline constexpr std::string_view k_swap_order_info = "/linear-swap-api/v1/swap_cross_order_info";
inline constexpr std::string_view k_swap_position_info = "/linear-swap-api/v1/swap_cross_position_info";
// no _cross here: mar_acct in the body selects cross or isolated
inline constexpr std::string_view k_swap_financial_record = "/linear-swap-api/v3/swap_financial_record";
inline constexpr std::string_view k_spot_open_orders = "/v1/order/openOrders";
inline constexpr std::string_view k_spot_order_by_client_id = "/v1/order/orders/getClientOrder";
inline constexpr std::string_view k_spot_accounts = "/v1/account/accounts";

inline constexpr std::string_view k_swap_contract_info = "/linear-swap-api/v1/swap_contract_info";
// every perpetual in one call when no contract_code is named
inline constexpr std::string_view k_swap_index = "/linear-swap-api/v1/swap_index";
inline constexpr std::string_view k_swap_open_interest = "/linear-swap-api/v1/swap_open_interest?business_type=swap";
inline constexpr std::string_view k_spot_symbols = "/v1/common/symbols";
inline constexpr std::string_view k_server_time = "/api/v1/timestamp";

} // namespace raikiri::venues::htx::ep
