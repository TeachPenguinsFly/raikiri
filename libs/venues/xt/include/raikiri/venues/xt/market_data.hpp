#pragma once

// What the public feed says a market did: its prints, the mark it quotes, the
// rolling 24-hour window, and the funding — which arrives by poll alone.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::xt {

// `m` is the AGGRESSOR's side, not the maker's: VERIFIED against a 100 ms book
// over 47 prints — "ASK" only ever printed at or below the best bid and "BID"
// only ever at or above the best ask, never the reverse. Reading it as a maker
// flag inverts every signed-flow series built on it.
//
// `a` is a count of CONTRACTS, like every quantity XT publishes on futures;
// spot names the same slot `q` and states base units in it.
struct WireTrade {
    std::string_view symbol;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireTrade, WireError> parse_trade(std::string_view frame,
                                                              MarketType market);

struct WireMarkPrice {
    std::string_view symbol;
    std::string_view mark_price;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view frame);

// ticker@SYMBOL: the rolling 24-hour window, restated in full every push, so
// an empty view here is a field XT did not send rather than a window nothing
// traded in.
//
// NOT agg_ticker, which carries the same keys and COPIES `a` INTO `v` —
// verified live on btc_usdt, eth_usdt and doge_usdt, where the two are equal
// to the digit while /public/q/agg-ticker states them apart. Its turnover is a
// contract count wearing a turnover's name.
//
// THE BASE COLUMN IS SPOT-ONLY. Spot puts base units in `q`; futures puts `a`
// in that slot and `a` counts CONTRACTS, which compares with nothing across
// venues. Both trees state the quote turnover in `v`.
struct WireTicker {
    std::string_view symbol;
    std::string_view volume_24h_base;  // spot `q`; always empty on futures
    std::string_view volume_24h_quote; // `v` on both trees
    std::string_view contracts_24h;    // futures `a`, in contracts
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame,
                                                                MarketType market);

// XT publishes no funding channel at all — `funding_rate@`, `funding@` and
// `fundingRate@` are each refused with "No enum constant ... Topic.x", so this
// arrives only by polling /public/q/funding-rate.
//
// rate_token is the RAW JSON token because XT ships fundingRate as a NUMBER
// rather than the digit string every other decimal here arrives as. It is
// plain decimal notation padded to a fixed scale ("-0.000002000000000000" —
// 18 fractional digits, all but six of them zero), which parse_decimal_e9
// accepts; 60 symbols were sampled and none used an exponent.
//
// interval_h is `collectionInternal` — XT's spelling — and its unit is HOURS.
struct WireFundingRate {
    std::string_view symbol;
    std::string_view rate_token;
    std::uint64_t next_collection_ms{0};
    std::int64_t interval_h{0};
};

[[nodiscard]] std::expected<WireFundingRate, WireError> parse_funding_rate(std::string_view body);

// What the two sources have said SO FAR about one contract's funding.
//
// XT SPLITS WHAT `FundingUpdate` FUSES: the rate is polled over REST and the
// mark streams on its own channel, and the shm cell is written whole.
// Publishing straight from either writes UNDEF over whatever the other one
// said, so the venue keeps both and publishes the merge. A field never stated
// stays UNDEF, which is not zero.
struct FundingMemo {
    FixedE9 rate{};
    PriceTicks mark{};
    UnixNanos next_funding{};
    std::uint32_t interval_s{0};
};

} // namespace raikiri::venues::xt
