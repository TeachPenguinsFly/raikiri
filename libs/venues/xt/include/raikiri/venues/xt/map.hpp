#pragma once

// Wire types -> domain types.
//
// Where XT's vocabulary becomes ours: strings become instrument-scaled
// integers, symbols become interned ids. Everything above this file is
// venue-agnostic; everything below it is XT-shaped.
//
// Three differences from the okx adapter are structural rather than cosmetic.
//
// The BBO is the top of a periodic top-N snapshot, not a dedicated channel.
// XT's agg_ticker publishes bid and ask PRICES with no sizes, so `depth` is
// the only channel that can fill a Bbo honestly.
//
// The price step is stated (minStepPrice) but the quantity step is not: XT
// gives quantityPrecision, a digit count, and the step is derived from it as
// an integer power of ten. That derivation is ours, not XT's.
//
// Quantities are counts of CONTRACTS, so contract_value carries what one is
// worth and cannot be pinned to 1.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/xt/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::xt {

// Venue milliseconds -> UnixNanos. A zero or absent venue timestamp yields
// local ingest time: a zero propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// A quantity step derived from a digit count: 10^-digits in 1e9 fixed point.
// Refuses a precision finer than the domain models, which is the same rule the
// tick size gets from parse_decimal_e9.
[[nodiscard]] std::optional<FixedE9> step_from_precision(std::int64_t digits) noexcept;

// Public market data ------------------------------------------------------

// Top of the snapshot. Returns nullopt when either side is empty, which is a
// real state on an illiquid contract and not a book with a zero-priced quote.
[[nodiscard]] std::optional<Bbo>
map_bbo(const WireDepth& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// mark_price is left undefined: XT ships mark on its own channel and funding
// by REST, so the two never arrive together and a zero here would read as a
// real price of zero.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireFundingRate& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Builds a domain instrument from a /public/symbol/list row. Returns nullopt
// for anything not tradable through the API, anything that is not the
// requested market, and any row whose steps failed to parse — an instrument
// with an unusable tick size must never enter the table, because every later
// price conversion divides by it.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

} // namespace raikiri::venues::xt
