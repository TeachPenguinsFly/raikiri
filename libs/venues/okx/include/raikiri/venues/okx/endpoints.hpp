#pragma once

// Every address this adapter speaks to, in one place.
//
// A path spelled inline is a path nobody can audit: when OKX moves one, or
// when a reader asks "what does this process actually call", the answer has to
// be greppable rather than scattered through a thousand lines of request
// building. It also keeps the demo split honest — a host chosen per call site
// is a host that will one day be chosen wrongly.

#include <string_view>

namespace raikiri::venues::okx::ep {

// --- hosts ---------------------------------------------------------------
// One REST host for live and demo alike: OKX switches an account to demo
// trading with an x-simulated-trading header, not with another hostname. The
// websocket is the opposite — demo is a different host serving a simulated
// book.
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

// --- sockets -------------------------------------------------------------
inline constexpr std::string_view k_ws_public = "/ws/v5/public";
inline constexpr std::string_view k_ws_private = "/ws/v5/private";

// --- order entry ---------------------------------------------------------
inline constexpr std::string_view k_order_create = "/api/v5/trade/order";
inline constexpr std::string_view k_order_cancel = "/api/v5/trade/cancel-order";
inline constexpr std::string_view k_order_amend = "/api/v5/trade/amend-order";
inline constexpr std::string_view k_order_cancel_batch = "/api/v5/trade/cancel-batch-orders";

// --- account and reconciliation -----------------------------------------
inline constexpr std::string_view k_orders_pending = "/api/v5/trade/orders-pending";
inline constexpr std::string_view k_fills = "/api/v5/trade/fills";
inline constexpr std::string_view k_positions = "/api/v5/account/positions";
// The account's own book, seven days deep. Funding and trading fees are
// rows on it, distinguished by `type`; `bills-archive` reaches further back
// and is not used, because a gap longer than a week is a restore, not a
// reconciliation.
inline constexpr std::string_view k_bills = "/api/v5/account/bills";

// --- reference data ------------------------------------------------------
inline constexpr std::string_view k_instruments = "/api/v5/public/instruments";
inline constexpr std::string_view k_mark_price = "/api/v5/public/mark-price";
inline constexpr std::string_view k_funding_rate = "/api/v5/public/funding-rate";
inline constexpr std::string_view k_books = "/api/v5/market/books";
inline constexpr std::string_view k_server_time = "/api/v5/public/time";

} // namespace raikiri::venues::okx::ep
