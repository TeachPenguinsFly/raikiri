#pragma once

// What the public feed says a market did: its prints on either socket, the
// ticker that doubles as the funding stream, and the REST read that is the
// only place a settlement boundary exists.

#include <raikiri/venues/mexc/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::mexc {

// bid1/ask1 are PRICES ONLY — MEXC publishes no top-of-book sizes on this
// channel, which is why the BBO is built from depth.full instead. What is here
// and nowhere else on the public socket is fundingRate, so this doubles as the
// funding stream. fair_price is MEXC's mark: it is the price liquidation and
// unrealised PnL are computed against.
struct WireTicker {
    std::string_view symbol;
    std::string_view bid_price;
    std::string_view ask_price;
    std::string_view last_price;
    std::string_view fair_price;
    std::string_view index_price;
    std::string_view funding_rate;
    // The rolling 24-hour window. MEXC restates the whole ticker on every
    // push — there are no deltas on this channel — so an empty view here is a
    // field MEXC did not send, never a window that traded nothing.
    //
    // `amount24` is turnover in the QUOTE currency; `volume24` counts
    // CONTRACTS, which the mapper scales by the contract size into base units.
    std::string_view volume_24h_quote;     // `amount24`
    std::string_view volume_24h_contracts; // `volume24`
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// `aggressor_bought` comes from `T`, which MEXC documents as the taker
// direction: 1 buy, 2 sell. Read the right way round here, so neither this nor
// the mapper inverts anything — binance's `m` says the opposite thing and
// reading the two the same way is how a signed flow series comes out backwards.
struct WireTrade {
    std::string_view symbol;
    std::string_view trade_id;
    std::string_view price;
    std::string_view qty; // CONTRACTS, not base units
    bool aggressor_bought{false};
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

// One frame can carry several prints, so this returns a span where the other
// channels return one struct.
[[nodiscard]] std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage);

// GET /contract/funding_rate/{symbol}. The websocket carries a rate but no
// settlement boundary at all, so this is the only place the schedule exists.
// collect_cycle_h is stated in HOURS.
struct WireFundingRate {
    std::string_view symbol;
    std::string_view funding_rate;
    std::string_view fair_price;
    std::uint64_t next_settle_ms{0};
    std::int64_t collect_cycle_h{0};
    std::uint64_t ts_ms{0};
    // The per-interval ceiling and floor, as fractions. Absent on the single
    // symbol answer for some contracts; empty then, and read as UNDEF.
    std::string_view max_rate;
    std::string_view min_rate;
};

[[nodiscard]] std::expected<WireFundingRate, WireError> parse_funding_rate(std::string_view body);

// GET /contract/funding_rate with NO symbol: every contract in one answer.
//
// THE ONLY PLACE MEXC STATES A SETTLEMENT INTERVAL. /contract/detail — the
// catalogue — carries no interval at all, so every perpetual was interned with
// 0, and an analysis that then assumed the common eight hours was wrong for
// most of the venue: read live 2026-09-24, 610 of 1,201 contracts settle every
// FOUR hours, 583 every eight, 7 every hour and one daily.
struct FundingRateBuffer {
    std::array<WireFundingRate, 2048> rows{};
};

[[nodiscard]] std::expected<std::span<const WireFundingRate>, WireError>
parse_funding_rates(std::string_view body, FundingRateBuffer& storage);

// The spot socket's prints, whose payload the envelope has already located.
// FIELD NUMBERS ARE STATED, NOT DERIVED — see envelope.hpp.
//
// One print: #1 price, #2 qty, #3 tradeType, #4 time ms, #5 id.
//
// tradeType 1 is the TAKER BUYING. VERIFIED against GET /api/v3/trades, which
// states isBuyerMaker outright: 24 prints matched by price, size and time,
// 24 agreed and none disagreed. Reading it the other way inverts every signed
// flow series built on this venue.
struct StreamTrade {
    std::string_view trade_id;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::uint64_t ts_ms{0};
};

struct StreamTradeBuffer {
    std::array<StreamTrade, k_max_trades> trades{};
};

[[nodiscard]] std::expected<std::span<const StreamTrade>, WireError>
parse_stream_trades(std::string_view payload, StreamTradeBuffer& storage);

// GET /api/v3/ticker/24hr with no symbol: every spot pair's rolling window in
// one answer. The spot socket's 24h ticker costs a subscription key per pair.
struct WireSpotTicker {
    std::string_view symbol;
    std::string_view volume_base;  // `volume`
    std::string_view volume_quote; // `quoteVolume`
    std::uint64_t close_ms{0};
};

struct SpotTickerBuffer {
    std::array<WireSpotTicker, 4096> rows{};
};

[[nodiscard]] std::expected<std::span<const WireSpotTicker>, WireError>
parse_spot_tickers(std::string_view body, SpotTickerBuffer& storage);

} // namespace raikiri::venues::mexc
