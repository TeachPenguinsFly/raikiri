#pragma once

// Wire types -> domain types.
//
// Where OKX's vocabulary becomes ours: strings become instrument-scaled
// integers, instrument ids become interned ids. Everything above this file is
// venue-agnostic; everything below it is OKX-shaped.
//
// Three differences from the binance adapter are structural rather than
// cosmetic.
//
// Sizes are CONTRACTS, not base units. A SWAP's ctVal says how much of
// ctValCcy one contract is worth, so contract_value is loaded from the
// catalogue instead of being pinned to 1 the way USD-M perps allow.
//
// Aggressor side arrives the right way round. OKX's `side` is the taker's
// direction, so there is no flip here — the binance mapper's inversion is a
// binance fact, not a shared one.
//
// The funding interval is derived, not assumed. /public/instruments does not
// carry it and OKX runs both 8h and 4h contracts, so it comes from the gap
// between the funding frame's own two settlement times.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/okx/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::okx {

// Venue milliseconds -> UnixNanos. A zero or absent venue timestamp yields
// local ingest time: a zero propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// Public market data ------------------------------------------------------

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Returns nullopt when the frame quotes no rate at all — a contract without
// funding must not be reported as one funding at zero. mark_price is left
// undefined because OKX ships mark on a separate channel; a zero there would
// read as a real price of zero.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireFundingRate& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Builds a domain instrument from an /public/instruments row. Returns nullopt
// for anything not live, anything that is not the requested market, and any
// row whose steps failed to parse — an instrument with an unusable tick size
// must never enter the table, because every later price conversion divides
// by it.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

} // namespace raikiri::venues::okx
