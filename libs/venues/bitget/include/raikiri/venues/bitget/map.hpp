#pragma once

// Wire types -> domain types.
//
// Where Bitget's vocabulary becomes ours: strings become instrument-scaled
// integers, symbols become interned ids. Everything above this file is
// venue-agnostic; everything below it is Bitget-shaped.
//
// Three differences from the okx adapter are structural. The tick is a
// PRODUCT of two catalogue fields, priceEndStep * 10^-pricePlace, and neither
// alone is the step (derivation in map.cpp). Sizes are base units, not
// contracts, so contract_value is 1 the way binance's USD-M perps allow and
// okx's SWAPs do not. And the funding interval is stated by the catalogue
// (fundInterval, in hours) rather than derived from the gap between two
// settlement stamps — 1h, 4h and 8h contracts are all live.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/bitget/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::bitget {

// Venue milliseconds -> UnixNanos. A zero or absent venue timestamp yields
// local ingest time: a zero propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// Public market data ------------------------------------------------------

// Both read the SAME `ticker` frame: top of book, mark and funding arrive in
// one element, so one subscription feeds two sinks.
[[nodiscard]] std::optional<Bbo>
map_bbo(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Returns nullopt when the frame quotes no rate at all — a contract without
// funding must not be reported as one funding at zero.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Returns nullopt for anything not tradable, anything that is not the
// requested market, and any row whose steps failed to parse: an instrument
// with an unusable tick must never enter the table, because every later price
// conversion divides by it.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market);

} // namespace raikiri::venues::bitget
