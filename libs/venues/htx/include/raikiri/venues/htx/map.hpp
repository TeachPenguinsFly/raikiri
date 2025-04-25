#pragma once

// Wire types -> domain types.
//
// Where HTX's vocabulary becomes ours: tokens become instrument-scaled
// integers, contract codes become interned ids. Everything above this file is
// venue-agnostic; everything below it is HTX-shaped.
//
// Three differences from the okx adapter are structural rather than cosmetic.
//
// parse_decimal_e9 is not the entry point. HTX writes prices and sizes as JSON
// numbers and spells the small ones in scientific notation ("1.00000000E-10"
// is PEPE-USDT's live tick), which that reader refuses outright. exact_e9
// below reads both spellings through an int128 mantissa and a base-10
// exponent, with no double anywhere.
//
// Sizes are CONTRACTS and the step is always one: an HTX order names an
// integer `volume`. contract_size turns a contract into base units, so it
// lands in contract_value rather than in lot_size.
//
// The funding interval is STATED, not inferred: settlement_period sits on the
// catalogue row, where okx derives it from consecutive funding frames.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/htx/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::htx {

// Venue milliseconds -> UnixNanos. A zero or absent venue timestamp yields
// local ingest time: a zero propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// Exact decimal-or-exponent token -> 1e9 fixed point. Refuses significance
// finer than 1e-9 rather than rounding it away: a tick flattened to zero makes
// every later price conversion a division by zero.
[[nodiscard]] std::optional<FixedE9> exact_e9(std::string_view token);

// The same reader, truncating below 1e-9 instead of refusing. Rates only: HTX
// pads every funding rate to eighteen fractional digits with a tail that is
// not always zero, so an exact read would present a running venue as silence.
// What remains is eight significant figures of a rate no order divides by.
[[nodiscard]] std::optional<FixedE9> rate_e9(std::string_view token);

// Public market data ------------------------------------------------------

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Returns nullopt when the frame quotes no rate at all — a contract without
// funding must not be reported as one funding at zero. mark_price stays
// undefined because this frame carries none. The interval comes from the
// instrument, which is where the catalogue put it.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireFunding& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Builds a domain instrument from a /swap_contract_info row. Returns nullopt
// for anything not listed, anything that is not a USDT-M swap, and any row
// whose tick or contract size failed to parse.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market);

} // namespace raikiri::venues::htx
