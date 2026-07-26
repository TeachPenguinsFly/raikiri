#pragma once

// What the public feed says a market did: its prints, the mark and index it
// quotes, and the funding it charges.
//
// The 24-hour window is NOT here. KuCoin states no turnover on any stream this
// adapter subscribes to; it rides the catalogue row instead, on WireContract.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::kucoin {

// `side` is already the AGGRESSOR's side — KuCoin states the taker direction
// directly, so nothing is inverted here or in the mapper. Binance's `m` says the
// opposite thing, and reading the two alike is how signed flow comes out backwards.
struct WireTrade {
    std::string_view symbol;
    std::string_view trade_id;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::int64_t sequence{0};
    std::uint64_t ts_ns{0};
};

[[nodiscard]] std::expected<WireTrade, WireError> parse_trade(std::string_view frame,
                                                              MarketType market);

[[nodiscard]] std::expected<WireTrade, WireError> parse_uta_trade(std::string_view frame);

struct WireMarkPrice {
    std::string_view symbol;
    std::string_view mark_price;
    std::string_view index_price;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view frame);

// The websocket form carries no settlement time; the REST form does but names
// the funding INDEX (".XBTUSDTMFPI8H") in `symbol`, useless for lookup — hence
// one DTO, two parsers, and a `symbol` only the websocket form fills.
//
// `granularity` means DIFFERENT THINGS on the two transports, so the mapper
// trusts neither: the stream republishes the live rate once a minute and says
// 60000, while REST says the 28800000 settlement period (verified live).
struct WireFundingRate {
    std::string_view symbol;
    std::string_view rate;
    std::string_view predicted_rate; // REST `predictedValue`; absent in practice
    std::uint64_t granularity_ms{0};
    std::uint64_t next_funding_ms{0}; // REST `fundingTime`; 0 on the websocket
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireFundingRate, WireError> parse_funding_rate(std::string_view frame);

// GET /api/v1/funding-rate/{symbol}/current. Seeds funding at subscribe time and
// is the only source that states the venue's own next settlement.
[[nodiscard]] std::expected<WireFundingRate, WireError>
parse_funding_current(std::string_view body);

// What the two subjects have said SO FAR about one contract's funding.
//
// KUCOIN SPLITS WHAT `FundingUpdate` FUSES: `funding.rate` and
// `mark.index.price` share the /contract/instrument topic and arrive as
// separate frames, and the shm cell is written whole. Publishing straight from
// either frame writes UNDEF over whatever the other one said, so the venue
// keeps both and publishes the merge. A field never stated stays UNDEF, which
// is not zero.
struct FundingMemo {
    FixedE9 rate{};
    FixedE9 predicted{};
    PriceTicks mark{};
    UnixNanos next_funding{};
    std::uint32_t interval_s{0};
};

} // namespace raikiri::venues::kucoin
