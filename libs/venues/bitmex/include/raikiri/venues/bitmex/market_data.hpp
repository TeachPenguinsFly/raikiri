#pragma once

// What the public tables say a market did: its top of book, its prints, and
// the `instrument` row that carries mark price, funding and the day's
// turnover.

#include <raikiri/venues/bitmex/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitmex {

// One row of `quote`. A side is present as a price/size PAIR or not at all.
//
// Rows are per liquidity POOL, and the aggregate is their sum at the best
// price — verified against the Aggregated orderBookL2_25 top, where
// 80200 + 2043500 is the 2123700 the venue reports at 64054.7.
struct WireQuote {
    std::string_view symbol;
    std::string_view pool; // "Primary" / "Secondary"
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::string_view ts; // ISO-8601
};

// A `partial` carries one row per pool; two pools exist today.
inline constexpr std::size_t k_max_quotes = 16;

struct QuoteBuffer {
    std::array<WireQuote, k_max_quotes> quotes{};
};

[[nodiscard]] std::expected<std::span<const WireQuote>, WireError>
parse_quotes(std::string_view frame, QuoteBuffer& storage);

// `side` is the AGGRESSOR's, stated as "Buy"/"Sell" — BitMEX names the taker
// directly, so nothing is inverted here or in the mapper. Binance's `m` says
// the opposite thing; reading the two the same way is how a signed flow series
// comes out backwards.
struct WireTrade {
    std::string_view symbol;
    std::string_view trade_id;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::string_view ts;
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

// One frame can carry several prints, so this returns a span where the
// single-row tables return one struct.
[[nodiscard]] std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage);

// A row of `instrument`. Only `symbol` is required: the common update carries
// three columns out of eighty.
//
// funding_ts is the NEXT settlement the current rate applies to, and
// funding_interval is a DURATION that BitMEX encodes as a timestamp — see
// map.hpp, where it is decoded.
struct WireInstrumentTick {
    std::string_view symbol;
    std::string_view mark_price;
    std::string_view funding_rate;
    std::string_view indicative_funding_rate;
    std::string_view funding_ts;
    std::string_view funding_interval;
    // EMPTY ON AN UPDATE, and empty is not zero: the `partial` states the whole
    // row and every frame after it carries only the columns that moved, so the
    // mark-price ticks that dominate this table carry neither.
    //
    // WHICH IS WHICH IS NOT THE NAMES. On XBTUSD homeNotional24h is the XBT
    // side and foreignNotional24h the USD side — verified by ratio against
    // volume24h in map.cpp, which is what an inverse contract makes worth
    // checking. volume24h itself is in CONTRACTS and compares with nothing.
    std::string_view home_notional_24h;    // base units
    std::string_view foreign_notional_24h; // quote units
    std::string_view ts;
};

// Reads data[0]: the subscription is always symbol-scoped, so a frame never
// carries a second instrument.
[[nodiscard]] std::expected<WireInstrumentTick, WireError>
parse_instrument_tick(std::string_view frame);

} // namespace raikiri::venues::bitmex
