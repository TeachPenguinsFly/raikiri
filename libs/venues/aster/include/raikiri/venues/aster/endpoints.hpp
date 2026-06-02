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
// MEASURED 2026-09-25: one socket subscribed a stream at a time is closed
// with 3003 "subscribed channels exceeds limit" at 200, the documented number.
inline constexpr std::size_t k_ws_max_streams = 200;

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

// --- what a call costs ---------------------------------------------------
// The venue meters WEIGHT, not requests, and counts one pool of it per IP:
// 2400 a minute, as its own exchangeInfo states.
//
// THE PRICES BELOW ARE MEASURED, NOT INHERITED. This API is wire-compatible
// with a much larger venue's — same paths, same field names, same
// `x-mbx-used-weight-1m` header — and the temptation is to carry that venue's
// price list across with the parsers. Two of them differ, both upward, and
// both on paths the funding poller walks every cycle:
//
//   MEASURED 2026-09-20, one call per path, deltas on the used-weight header:
//     /fapi/v1/time                    1
//     /fapi/v1/exchangeInfo            1
//     /fapi/v1/premiumIndex   (table) 10   <- the other venue charges 1
//     /fapi/v1/fundingInfo            10   <- the other venue charges 1
//     /fapi/v1/depth       (limit 100) 5
//     /fapi/v1/depth      (limit 1000) 20
//     /fapi/v1/ticker/24hr    (table) 40
//     /fapi/v1/ticker/24hr  (1 symbol) 1
//
// Priced at 1, the gate would admit ten times the funding traffic the venue
// actually charges for and find out at the ban rather than at the budget.

// GET /fapi/v1/depth, whose weight is a ladder in `limit`: 2 for 5/10/20/50,
// 5 for 100, 10 for 500, 20 for 1000.
//
// A limit off the ladder falls to its dearest rung. The venue accepts no other
// value, so this is either a call nobody costed or a target this could not
// read — and undercharging either is what the ladder exists to prevent.
[[nodiscard]] constexpr std::uint32_t depth_weight(std::uint64_t limit) noexcept {
    if (limit == 5 || limit == 10 || limit == 20 || limit == 50) {
        return 2;
    }
    if (limit == 100) {
        return 5;
    }
    if (limit == 500) {
        return 10;
    }
    return 20;
}

// The `limit` a target states, or 0 when it states none. Not a query parser:
// it reads the one parameter on this API that changes a price.
[[nodiscard]] constexpr std::uint64_t stated_limit(std::string_view target) noexcept {
    constexpr std::string_view key = "limit=";
    for (auto at = target.find(key); at != std::string_view::npos; at = target.find(key, at + 1)) {
        // `?limit=` or `&limit=`, so a parameter merely ENDING in "limit" is
        // not read as this one.
        if (at == 0 || (target[at - 1] != '?' && target[at - 1] != '&')) {
            continue;
        }
        std::uint64_t value = 0;
        for (auto i = at + key.size(); i < target.size(); ++i) {
            const char digit = target[i];
            if (digit < '0' || digit > '9') {
                break;
            }
            value = value * 10 + static_cast<std::uint64_t>(digit - '0');
            if (value > 100'000) {
                return 0; // past anything the venue accepts; charged as unstated
            }
        }
        return value;
    }
    return 0;
}

// The dearest unsigned call measured: the whole 24-hour table. A target this
// file does not know is charged at it, because a path nobody costed must not
// be the free one.
inline constexpr std::uint32_t k_max_unsigned_weight = 40;

[[nodiscard]] constexpr std::uint32_t market_data_weight(std::string_view target) noexcept {
    const auto path = target.substr(0, target.find('?'));
    if (path == k_depth) {
        return depth_weight(stated_limit(target));
    }
    // Weight 1 for one symbol, 40 for the table. Nothing asks per symbol.
    if (path == k_ticker_24h) {
        return k_max_unsigned_weight;
    }
    // Both 10, and both measured rather than assumed — see the note above.
    if (path == k_premium_index || path == k_funding_info) {
        return 10;
    }
    if (path == k_exchange_info || path == k_server_time) {
        return 1;
    }
    return k_max_unsigned_weight;
}

} // namespace raikiri::venues::aster::ep
