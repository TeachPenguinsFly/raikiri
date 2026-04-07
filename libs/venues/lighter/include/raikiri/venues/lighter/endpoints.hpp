#pragma once

// Every address this adapter speaks to, in one place.
//
// MARKET DATA AND FUNDING ONLY, and no credentials. Lighter is a zk-rollup:
// an order is a transaction signed with the venue's own API-key scheme and
// submitted through sendTx, which no signer in this codebase produces.

#include <cstdint>
#include <string_view>

namespace raikiri::venues::lighter::ep {

// --- hosts ---------------------------------------------------------------
// The testnet is real (testnet.zklighter.elliot.ai) and carries its own
// market ids, so the flag picks a host rather than being ignored.
inline constexpr std::string_view k_rest_live = "mainnet.zklighter.elliot.ai";
inline constexpr std::string_view k_rest_test = "testnet.zklighter.elliot.ai";

[[nodiscard]] constexpr std::string_view rest_host(bool testnet) noexcept {
    return testnet ? k_rest_test : k_rest_live;
}

[[nodiscard]] constexpr std::string_view stream_host(bool testnet) noexcept {
    return rest_host(testnet);
}

// --- sockets -------------------------------------------------------------
inline constexpr std::string_view k_ws_stream = "/stream";

// --- reference data ------------------------------------------------------
// Both catalogues in one body — `order_book_details` for perps and
// `spot_order_book_details` for spot — with the funding clamps beside the
// decimals, which /api/v1/orderBooks does not carry.
inline constexpr std::string_view k_order_book_details = "/api/v1/orderBookDetails";

// NOT READ, and named so nobody reads it by mistake. /api/v1/funding-rates is
// a comparison table: one row per (market, exchange) for lighter, binance,
// bybit and hyperliquid, every rate restated per 8 hours. Its "lighter" rows
// are this venue's own current rate times eight (210 of 210 on 2026-09-24);
// every other row is another venue's number under a lighter market_id.
inline constexpr std::string_view k_funding_comparison = "/api/v1/funding-rates";

// --- what a call costs ---------------------------------------------------
// STATED, per IP, on apidocs.lighter.xyz/docs/rate-limits (updated
// 2026-09-23): an unauthenticated caller is a Standard account, allowed 60
// requests a rolling minute and unweighted. The weighted table — 300 for
// orderBookDetails, 600 for recentTrades — prices the Premium tier's 24 000,
// and 24 000 / 300 = 80 is above 60, so the flat 60 is what binds here.
//
// A REST refusal is 429 (sometimes 405) and it throttles the SOCKETS of the
// same IP too, which is why this adapter asks REST for the catalogue once and
// takes everything else off the stream. The firewall's cooldown is a flat 60 s.
inline constexpr std::uint32_t k_rest_requests_per_minute = 60;
inline constexpr std::uint64_t k_rest_cooldown_ns = 60ULL * 1'000'000'000ULL;

} // namespace raikiri::venues::lighter::ep
