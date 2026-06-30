#pragma once

// Every address this adapter speaks to, in one place.
//
// A path spelled inline is a path nobody can audit: when the venue moves one,
// or when a reader asks "what does this process actually call", the answer has
// to be greppable rather than scattered through a thousand lines of request
// building.
//
// Everything here is the USD-margined perpetual API (fapi). This adapter reads
// MARKET DATA AND FUNDING ONLY and holds no credentials: the venue signs order
// entry with an EVM wallet signature rather than an HMAC secret, which is a
// different signer from every other adapter here and is deliberately not built.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::aster::ep {

// --- hosts ---------------------------------------------------------------
// NO TESTNET. Neither testnet.asterdex.com nor fapi-testnet.asterdex.com
// resolves (checked 2026-09-20), so a testnet flag has nowhere to point. The
// accessors below ignore it rather than naming a host that does not exist —
// a dead hostname fails at connect time, far from the config that asked.
inline constexpr std::string_view k_rest_live = "fapi.asterdex.com";
inline constexpr std::string_view k_stream_live = "fstream.asterdex.com";

[[nodiscard]] constexpr std::string_view rest_host(bool testnet) noexcept {
    (void)testnet;
    return k_rest_live;
}

[[nodiscard]] constexpr std::string_view stream_host(bool testnet) noexcept {
    (void)testnet;
    return k_stream_live;
}

// --- sockets -------------------------------------------------------------
inline constexpr std::string_view k_ws_combined = "/stream";

// --- reference data ------------------------------------------------------
inline constexpr std::string_view k_exchange_info = "/fapi/v1/exchangeInfo";
inline constexpr std::string_view k_premium_index = "/fapi/v1/premiumIndex";
inline constexpr std::string_view k_ticker_24h = "/fapi/v1/ticker/24hr";
inline constexpr std::string_view k_depth = "/fapi/v1/depth";
inline constexpr std::string_view k_server_time = "/fapi/v1/time";
// The settlement schedule, and the only place the venue states it:
// exchangeInfo carries every other trading rule for a perp but not how often
// it funds.
inline constexpr std::string_view k_funding_info = "/fapi/v1/fundingInfo";

} // namespace raikiri::venues::aster::ep
