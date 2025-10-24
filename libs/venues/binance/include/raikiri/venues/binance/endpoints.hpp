#pragma once

// Every address this adapter speaks to, in one place.
//
// A path spelled inline is a path nobody can audit: when binance moves one, or
// when a reader asks "what does this process actually call", the answer has to
// be greppable rather than scattered through a thousand lines of request
// building. It also keeps the testnet split honest — a host chosen per call
// site is a host that will one day be chosen wrongly.
//
// Everything here is the USD-margined futures API (fapi). Binance spot is
// another host behind another API and this adapter never reaches it.

#include <cstdint>
#include <string_view>

namespace raikiri::venues::binance::ep {

// --- hosts ---------------------------------------------------------------
inline constexpr std::string_view k_rest_live = "fapi.binance.com";
inline constexpr std::string_view k_rest_test = "testnet.binancefuture.com";
inline constexpr std::string_view k_stream_live = "fstream.binance.com";
inline constexpr std::string_view k_stream_test = "fstream.binancefuture.com";

[[nodiscard]] constexpr std::string_view rest_host(bool testnet) noexcept {
    return testnet ? k_rest_test : k_rest_live;
}

[[nodiscard]] constexpr std::string_view stream_host(bool testnet) noexcept {
    return testnet ? k_stream_test : k_stream_live;
}

// --- sockets -------------------------------------------------------------
inline constexpr std::string_view k_ws_combined = "/stream";
// The user data socket addresses itself with the listen key: this prefix is
// only half a target and the key is appended at the call site.
inline constexpr std::string_view k_ws_user_prefix = "/ws/";

// --- order entry ---------------------------------------------------------
// One path for four verbs: POST places, DELETE cancels, PUT amends, GET reads.
inline constexpr std::string_view k_order = "/fapi/v1/order";

// --- account and reconciliation -----------------------------------------
inline constexpr std::string_view k_open_orders = "/fapi/v1/openOrders";
inline constexpr std::string_view k_income = "/fapi/v1/income";
inline constexpr std::string_view k_user_trades = "/fapi/v1/userTrades";
inline constexpr std::string_view k_position_risk = "/fapi/v3/positionRisk";
// THE WALLET IS READ HERE AND NOT FROM /fapi/v2/balance, because this is the
// only answer that also states `multiAssetsMargin` — and without the mode, the
// per-asset `availableBalance` on either endpoint cannot be published at all:
// in multi-assets mode binance repeats ONE account-wide figure on every row.
//
// v3 of this path exists and is the newer revision of the same read, and it
// DROPPED multiAssetsMargin; /fapi/v1/accountConfig still states it, but as a
// second call. The mode and the figures it qualifies have to come out of one
// snapshot — two reads can straddle a mode switch, and the row that lands on
// the wrong side of it claims a whole account's margin as free BTC.
//
// Asked at v2 while positionRisk above is asked at v3: binance versions these
// endpoints one at a time, and the mismatch is the current shape of the API
// rather than a stale path.
inline constexpr std::string_view k_account = "/fapi/v2/account";
// POST opens, PUT keeps alive, DELETE closes.
inline constexpr std::string_view k_listen_key = "/fapi/v1/listenKey";

// --- reference data ------------------------------------------------------
inline constexpr std::string_view k_exchange_info = "/fapi/v1/exchangeInfo";
inline constexpr std::string_view k_premium_index = "/fapi/v1/premiumIndex";
// Every symbol in one answer. Weight 40 for the whole table against 1 per
// symbol, which is why nothing here asks per symbol.
inline constexpr std::string_view k_ticker_24h = "/fapi/v1/ticker/24hr";
inline constexpr std::string_view k_depth = "/fapi/v1/depth";
inline constexpr std::string_view k_server_time = "/fapi/v1/time";

// --- what a call costs ---------------------------------------------------
// Binance meters WEIGHT, not requests, and counts one pool of it per IP:
// 2400 a minute across every path above. A market-data gate that charged a
// flat 1 would price a 571-instrument seed round at 571 where binance prices
// it at 11420, and would not refuse anything until long after the ban.
//
// Only the UNSIGNED calls are priced here. What the signed path costs was
// measured from `X-MBX-USED-WEIGHT-1M` and lives in config/venues.yaml, and
// duplicating it here would give one venue two answers.

// GET /fapi/v1/depth, whose documented weight is a ladder in `limit`: 2 for
// 5/10/20/50, 5 for 100, 10 for 500, 20 for 1000.
//
// A limit off the ladder falls to its dearest rung. Binance accepts no other
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
                return 0; // past anything binance accepts; charged as unstated
            }
        }
        return value;
    }
    return 0;
}

// The dearest unsigned call binance states: the whole 24-hour table. A target
// this file does not know is charged at it, because a path nobody costed must
// not be the free one.
inline constexpr std::uint32_t k_max_unsigned_weight = 40;

// Every figure below is from binance's USD-M futures documentation, endpoint
// by endpoint.
[[nodiscard]] constexpr std::uint32_t market_data_weight(std::string_view target) noexcept {
    const auto path = target.substr(0, target.find('?'));
    if (path == k_depth) {
        return depth_weight(stated_limit(target));
    }
    // Weight 1 for one symbol, 40 for the table. Nothing asks per symbol.
    if (path == k_ticker_24h) {
        return k_max_unsigned_weight;
    }
    // Weight 1, table or single symbol alike: unlike the 24-hour figures,
    // binance states no surcharge for omitting the symbol here.
    if (path == k_premium_index) {
        return 1;
    }
    // Weight 1 each, and each is asked once per process start.
    if (path == k_exchange_info || path == k_server_time) {
        return 1;
    }
    return k_max_unsigned_weight;
}

} // namespace raikiri::venues::binance::ep
