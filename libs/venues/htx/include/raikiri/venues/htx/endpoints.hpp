#pragma once

// Every address this adapter speaks to, in one place.
//
// A path spelled inline is a path nobody can audit: when HTX moves one, or
// when a reader asks "what does this process actually call", the answer has to
// be greppable rather than scattered through a thousand lines of request
// building.
//
// THE HOST IS A TERM OF THE SIGNATURE here (METHOD \n host \n path \n query),
// so spot and derivatives are not interchangeable names for one API: a request
// built for one and sent to the other authenticates as a bad key rather than
// as a wrong address.

#include <string_view>

namespace raikiri::venues::htx::ep {

// --- hosts ---------------------------------------------------------------
// HTX publishes no testnet on either product; the config's flag has no host to
// select.
inline constexpr std::string_view k_rest_futures = "api.hbdm.com";
inline constexpr std::string_view k_rest_spot = "api.huobi.pro";
inline constexpr std::string_view k_stream_futures = "api.hbdm.com";
inline constexpr std::string_view k_stream_spot = "api.huobi.pro";

// --- sockets -------------------------------------------------------------
// Two paths on the derivatives host speaking two dialects: the market socket
// takes {"sub":"market.{code}.{topic}"} and the notification one takes
// {"op":"sub","topic":...}. VERIFIED: funding_rate is refused on the market
// socket, so notify is the only funding push HTX has.
inline constexpr std::string_view k_ws_market = "/linear-swap-ws";
inline constexpr std::string_view k_ws_notify = "/linear-swap-notification";
inline constexpr std::string_view k_ws_spot_private = "/ws/v2";

// --- order entry ---------------------------------------------------------
// Every swap path here and below is the CROSS-margin family, matching
// rest.hpp's k_margin_mode and ws.hpp's *_cross topics. Isolated margin is a
// parallel family spelled WITHOUT the infix (swap_order, swap_cancel,
// swap_openorders, swap_order_info, swap_position_info); moving to it means
// changing all three places in one edit.
inline constexpr std::string_view k_swap_order = "/linear-swap-api/v1/swap_cross_order";
// One path for the single cancel and the batch alike; the body says which.
inline constexpr std::string_view k_swap_cancel = "/linear-swap-api/v1/swap_cross_cancel";
inline constexpr std::string_view k_spot_order_place = "/v1/order/orders/place";
// The venue order id is an INFIX: /v1/order/orders/{id}/submitcancel.
inline constexpr std::string_view k_spot_orders = "/v1/order/orders";
inline constexpr std::string_view k_spot_submit_cancel_suffix = "/submitcancel";
inline constexpr std::string_view k_spot_cancel_by_client_id =
    "/v1/order/orders/submitCancelClientOrder";
inline constexpr std::string_view k_spot_batch_cancel = "/v1/order/orders/batchcancel";

// --- account and reconciliation -----------------------------------------
inline constexpr std::string_view k_swap_open_orders = "/linear-swap-api/v1/swap_cross_openorders";
inline constexpr std::string_view k_swap_order_info = "/linear-swap-api/v1/swap_cross_order_info";
inline constexpr std::string_view k_swap_position_info =
    "/linear-swap-api/v1/swap_cross_position_info";
// The account's own ledger, and the ONE private swap path with no `_cross` in
// it: cross and isolated are selected by the `mar_acct` body field here, not
// by a parallel endpoint family. There is no swap_cross_financial_record.
inline constexpr std::string_view k_swap_financial_record =
    "/linear-swap-api/v3/swap_financial_record";
inline constexpr std::string_view k_spot_open_orders = "/v1/order/openOrders";
inline constexpr std::string_view k_spot_order_by_client_id = "/v1/order/orders/getClientOrder";
// Every spot order needs the account id this returns, so it is fetched before
// the first one can be built.
inline constexpr std::string_view k_spot_accounts = "/v1/account/accounts";

// --- reference data ------------------------------------------------------
inline constexpr std::string_view k_swap_contract_info = "/linear-swap-api/v1/swap_contract_info";
inline constexpr std::string_view k_spot_symbols = "/v1/common/symbols";
// The derivatives clock; spot's own is not read.
inline constexpr std::string_view k_server_time = "/api/v1/timestamp";

} // namespace raikiri::venues::htx::ep
