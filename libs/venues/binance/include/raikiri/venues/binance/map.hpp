#pragma once

// Wire types -> domain types.
//
// Where binance's vocabulary becomes ours: strings become instrument-scaled
// integers, symbols become interned ids. Everything above this file is
// venue-agnostic; everything below it is binance-shaped.
//
// Two differences from the bybit adapter are structural rather than cosmetic.
//
// BBO arrives ready-made. Binance publishes a dedicated bookTicker stream, so
// top-of-book needs no local book and no resynchronisation — the depth stream
// exists for full-book work that does not ship yet. Bybit has no such stream,
// which is why it maintains a book to produce the same event.
//
// Aggressor side is inverted on the wire. `m` is "the buyer was the maker",
// so m == true means the AGGRESSOR SOLD. The wire struct keeps binance's name
// and meaning; the flip happens here, once.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/binance/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::binance {

// Venue milliseconds -> UnixNanos. A zero or absent venue timestamp yields
// local ingest time: a zero propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// Public market data ------------------------------------------------------

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBookTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireAggTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// markPriceUpdate carries mark, index and funding together. Returns nullopt
// when the frame quotes no funding rate at all — a contract without funding
// must not be reported as one funding at zero.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireMarkPrice& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Builds a domain instrument from an exchangeInfo row. Returns nullopt for
// anything not currently trading, anything that is not the requested market,
// and any row whose filters failed to parse — an instrument with an unusable
// tick size must never enter the table, because every later price conversion
// divides by it.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

} // namespace raikiri::venues::binance
