#pragma once

// Every address this adapter speaks to, in one place.
//
// A path spelled inline is a path nobody can audit: when KuCoin moves one, or
// when a reader asks "what does this process actually call", the answer has to
// be greppable rather than scattered through a thousand lines of request
// building. It also keeps the two-host split honest — spot and futures are
// different hostnames spelling the same intent differently, and a host chosen
// per call site is a host that will one day be chosen wrongly.

#include <string_view>

namespace raikiri::venues::kucoin::ep {

// --- hosts ---------------------------------------------------------------
// The futures host is also the market-data host. There is no testnet: KuCoin
// retired its sandbox and the live hosts are the only ones that answer.
inline constexpr std::string_view k_rest_futures = "api-futures.kucoin.com";
inline constexpr std::string_view k_rest_spot = "api.kucoin.com";

// --- sockets -------------------------------------------------------------
// KuCoin names no socket address of its own: a bullet call answers with the
// host and target to connect to, and the adapter splits what it is handed.
// Same paths on both hosts — which host they reach decides which account's
// stream the token opens.
inline constexpr std::string_view k_bullet_public = "/api/v1/bullet-public";
inline constexpr std::string_view k_bullet_private = "/api/v1/bullet-private";

// --- order entry ---------------------------------------------------------
inline constexpr std::string_view k_spot_orders = "/api/v1/hf/orders";
inline constexpr std::string_view k_spot_order_amend = "/api/v1/hf/orders/alter";
inline constexpr std::string_view k_futures_orders = "/api/v1/orders";
// Cancelling by client order id addresses the order through this segment
// instead of appending the venue's id: .../orders/client-order/{clientOid}.
inline constexpr std::string_view k_client_order_segment = "client-order/";

// --- account and reconciliation -----------------------------------------
inline constexpr std::string_view k_spot_orders_active = "/api/v1/hf/orders/active";
inline constexpr std::string_view k_spot_active_symbols = "/api/v1/hf/orders/active/symbols";
inline constexpr std::string_view k_spot_fills = "/api/v1/hf/fills";
inline constexpr std::string_view k_futures_fills = "/api/v1/fills";
inline constexpr std::string_view k_positions = "/api/v1/positions";
// Futures only, and per settlement currency: the spot wallet is a different
// shape on the other host.
inline constexpr std::string_view k_account_overview = "/api/v1/account-overview";

// --- reference data ------------------------------------------------------
// Serves perpetuals and dated contracts alike; only `type` tells them apart.
inline constexpr std::string_view k_contracts_active = "/api/v1/contracts/active";
// Spot host.
inline constexpr std::string_view k_spot_symbols = "/api/v2/symbols";
// The symbol sits MID-PATH: k_funding_rate + "/" + symbol + k_funding_rate_current.
inline constexpr std::string_view k_funding_rate = "/api/v1/funding-rate";
inline constexpr std::string_view k_funding_rate_current = "/current";
inline constexpr std::string_view k_level2_snapshot = "/api/v1/level2/snapshot";
inline constexpr std::string_view k_server_time = "/api/v1/timestamp";

} // namespace raikiri::venues::kucoin::ep
