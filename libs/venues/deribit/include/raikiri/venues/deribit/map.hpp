#pragma once

// Wire types -> domain types.
//
// Where Deribit's vocabulary becomes ours: JSON number tokens become
// instrument-scaled integers, names become interned ids. Three differences
// from the okx adapter are structural rather than cosmetic.
//
// Decimals arrive as JSON numbers written by a float formatter, so "0.5",
// "1e-05" and "9.5e4" all appear on the wire and parse_decimal_e9 alone
// cannot read a single catalogue row.
//
// There is no perpetual kind. BTC-PERPETUAL is kind "future" with
// settlement_period "perpetual", so Perp and Future come out of the same
// endpoint and the mapper is what separates them.
//
// Options load, filling the OptionInstrument seam nothing exercised before.
// Both that and the INVERSE contract_value semantics are decided in
// map_instrument; those two comments are the ones worth reading before
// touching this file.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/deribit/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::deribit {

// parse_decimal_e9 plus an exponent suffix, and exact only: a token needing
// more than nine fractional digits is refused, never rounded. Exposed because
// that refusal is worth pinning by test.
[[nodiscard]] std::optional<FixedE9> parse_number_e9(std::string_view token) noexcept;

// A zero or absent venue timestamp yields local ingest time: a zero reaching
// a latency histogram poisons it.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// Public market data ------------------------------------------------------

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Returns nullopt when the ticker quotes no funding at all: a dated future
// ships no funding fields, while a perpetual quoting exactly 0.0 was captured
// live, so absence and zero must not be read the same way.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Builds a domain instrument from a /public/get_instruments row. Returns
// nullopt for anything inactive, linear, of another market, or whose steps
// failed to parse — an instrument with an unusable tick size must never enter
// the table, because every later price conversion divides by it.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

} // namespace raikiri::venues::deribit
