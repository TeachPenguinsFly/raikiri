#pragma once

// Every address this adapter speaks to, in one place.
//
// Spot and perpetuals share one host, one catalogue and one socket: a market's
// kind is a field on its row (`marketType`), and a perpetual is simply a
// symbol ending in _PERP. This adapter reads MARKET DATA AND FUNDING ONLY and
// holds no credentials.

#include <cstdint>
#include <string_view>

namespace raikiri::venues::backpack::ep {

// --- hosts ---------------------------------------------------------------
// NO TESTNET. Neither api.testnet.backpack.exchange nor
// ws.testnet.backpack.exchange resolves (checked 2026-09-24), so the flag has
// nowhere to point and both accessors ignore it rather than naming a host that
// fails at connect time, far from the config that asked.
inline constexpr std::string_view k_rest_live = "api.backpack.exchange";
inline constexpr std::string_view k_stream_live = "ws.backpack.exchange";

[[nodiscard]] constexpr std::string_view rest_host(bool testnet) noexcept {
    (void)testnet;
    return k_rest_live;
}

[[nodiscard]] constexpr std::string_view stream_host(bool testnet) noexcept {
    (void)testnet;
    return k_stream_live;
}

// --- sockets -------------------------------------------------------------
// The bare host. `/stream` is the pre-2024 API and answers 400.
inline constexpr std::string_view k_ws_path = "/";

// --- reference data ------------------------------------------------------
inline constexpr std::string_view k_markets = "/api/v1/markets";
inline constexpr std::string_view k_depth = "/api/v1/depth";
inline constexpr std::string_view k_tickers = "/api/v1/tickers";

// --- what a call costs ---------------------------------------------------
// Backpack meters REQUESTS, not weight: 2000 a minute across the standard
// REST routes, and 30 a minute on the "historical" ones (klines, trade
// history, funding history), none of which this adapter calls. Stated only in
// the support FAQ — no response carries a limit or a remainder header, so
// nothing here can read the allowance back and the count has to be our own.
//
// MEASURED 2026-09-24, unauthenticated: 300 depth snapshots in 5.6 s from 20
// parallel connections, all 200, no header naming a budget. That bounds the
// burst from below and says nothing about the ceiling, which was not driven to
// a 429 on purpose.
inline constexpr std::uint32_t k_requests_per_minute = 2000;

// Every path above is one request against that allowance. The historical
// routes are not priced here because they are not dearer, they are a second
// allowance; a caller that starts using one needs a second bucket.
inline constexpr std::uint32_t k_request_cost = 1;

} // namespace raikiri::venues::backpack::ep
