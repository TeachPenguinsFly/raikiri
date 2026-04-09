#pragma once

// Wire types -> domain types.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/backpack/book.hpp>
#include <raikiri/venues/backpack/catalogue.hpp>
#include <raikiri/venues/backpack/market_data.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::backpack {

// Venue microseconds -> UnixNanos. A zero stamp yields `fallback`: a zero
// propagating into a latency histogram poisons it.
[[nodiscard]] UnixNanos us_to_unix(std::uint64_t us, UnixNanos fallback) noexcept;

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBookTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// The touch of a REST depth snapshot, for an instrument whose bookTicker has
// not spoken yet. Best bid is the highest bid wherever the venue put it.
[[nodiscard]] std::optional<Bbo>
map_touch(const WireDepthSnapshot& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WirePrint& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Nullopt for a frame that states no rate — every spot pair's markPrice.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireMarkPrice& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// The row carries no timestamp of its own, so `received` is the event time.
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos received);

// Nullopt for any row not Open, not the requested market, or whose sizes do
// not parse.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireMarket& wire, MarketType market);

} // namespace raikiri::venues::backpack
