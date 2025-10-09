#pragma once

// What the public feed says a market did: its prints, the mark, the rolling
// 24-hour window — and the funding, which BingX alone among these venues does
// not stream at all and which therefore arrives as a REST read.

#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bingx {

// `m` says whether the BUYER was the maker, so the aggressor is the opposite
// side — the inverse of okx's `side`, which names the taker directly. VERIFIED
// against 166 live prints matched to the concurrent bookTicker: 163 agreed, 3
// disagreed on a quote that had already moved. Reading the two venues the same
// way is how a signed flow series comes out backwards. There is no trade id.
struct WireTrade {
    std::string_view symbol;
    std::string_view price;
    std::string_view qty;
    std::uint64_t trade_ms{0};
    bool buyer_is_maker{false};
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

// One frame can carry several prints, hence a span — the swap tree sends an
// array. Spot sends a single object under the same key, which is why this
// accepts either rather than two functions telling the caller apart.
[[nodiscard]] std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage);

struct WireMarkPrice {
    std::string_view symbol;
    std::string_view mark_price;
    std::uint64_t event_ms{0};
};

[[nodiscard]] std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view frame);

// {sym}@ticker, the venue's rolling 24-hour window. It is the only statistic
// on either socket — BingX streams no funding at all, so nothing else shares
// this channel and nothing downstream depends on it arriving.
//
// `v` is the window in BASE units and `q` the same window in QUOTE. VERIFIED
// against the REST ticker taken a minute apart: volume 22266.0100 against
// quoteVolume 1795681423.88, a quotient of 80646 inside that frame's
// 78949-82264 low-high band. Both trees push a whole snapshot every second
// rather than a delta, and both columns are optional even so — empty is not
// zero. The perpetual tree QUOTES both numbers and spot sends them bare, which
// is why they are read as scalars rather than as strings.
struct WireTicker {
    std::string_view symbol;
    std::string_view volume;       // `v`, base units
    std::string_view quote_volume; // `q`, quote units
    std::uint64_t event_ms{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// /quote/premiumIndex is the ONLY funding source BingX has: @fundingRate,
// @premiumIndex and @markPriceAndFundingRate are each refused with 80015
// (VERIFIED), so funding is a poll. `last_funding_rate` is the rate in force
// for the NEXT settlement, not one already charged.
struct WirePremiumIndex {
    std::string_view symbol;
    std::string_view mark_price;
    std::string_view index_price;
    std::string_view last_funding_rate;
    std::uint64_t next_funding_ms{0};
    std::int64_t funding_interval_hours{0};
};

[[nodiscard]] std::expected<WirePremiumIndex, WireError> parse_premium_index(std::string_view body);

} // namespace raikiri::venues::bingx
