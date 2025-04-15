#pragma once

// Wire types -> domain types.
//
// Where Gate's vocabulary becomes ours: strings become instrument-scaled
// integers, contract names become interned ids. Everything above this file is
// venue-agnostic; everything below it is Gate-shaped.
//
// Three differences from the binance adapter are structural rather than
// cosmetic.
//
// Sizes are CONTRACTS, not base units. `quanto_multiplier` says how much of
// the base asset one contract is, so contract_value comes from the catalogue
// instead of being pinned to 1 the way USD-M perps allow. The lot step is one
// whole contract: public market data quotes integer sizes on every contract,
// verified live including the `enable_decimal` ones.
//
// Aggressor side arrives as the sign of the size and the wire layer already
// split it, so there is no flip here — binance's inversion is a binance fact.
//
// The funding interval is stated, not derived: Gate carries funding_interval
// on both the catalogue and the ticker channel.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/gate/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::gate {

// Venue milliseconds -> UnixNanos. A zero or absent venue timestamp yields
// local ingest time: a zero propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// Venue SECONDS -> UnixNanos, for the funding fields. Reading one of these as
// milliseconds puts the settlement in 1970; reading a millisecond field as
// seconds puts it 30 millennia out. Both are silent.
[[nodiscard]] UnixNanos s_to_unix(std::uint64_t seconds) noexcept;

// Public market data ------------------------------------------------------

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBookTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Returns nullopt when the frame quotes no rate at all — a contract without
// funding must not be reported as one funding at zero. Unlike okx, mark price
// rides the same channel and is filled in when it lands on the price grid.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Builds a domain instrument from a /futures/{settle}/contracts row. Returns
// nullopt for anything delisting or not trading, anything that is not a perp,
// and any row whose steps failed to parse — an instrument with an unusable
// tick size must never enter the table, because every later price conversion
// divides by it.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market);

} // namespace raikiri::venues::gate
