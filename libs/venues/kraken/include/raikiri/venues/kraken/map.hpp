#pragma once

// Wire types -> domain types: numeric tokens become instrument-scaled
// integers, product ids become interned ids.
//
// Values arrive in two spellings, so parse_number_e9 replaces
// parse_decimal_e9 everywhere — the latter cannot read exponents at all.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/kraken/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::kraken {

// Venue ms -> UnixNanos. A zero or absent venue timestamp yields ingest time:
// a zero propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// Exact decimal-or-exponent token -> 1e9 fixed point. Reads both spellings
// Kraken uses for one field and refuses significance below 1e-9 rather than
// rounding it away. No double at any step.
[[nodiscard]] std::optional<FixedE9> parse_number_e9(std::string_view text) noexcept;

// contractValueTradePrecision -> quantity step, NEGATED: 4 means 0.0001,
// -3 means 1000. VERIFIED against live books — PF_PEPEUSD (-3) quotes sizes
// that are all multiples of 1000 and not 10000, PF_TURBOUSD (-2) multiples
// of 100 and not 1000. nullopt outside what 1e9 can hold.
[[nodiscard]] std::optional<FixedE9> step_from_precision(std::int64_t precision) noexcept;

// Public market data ------------------------------------------------------

// nullopt for a suspended market: its last quote is not tradeable, and a
// strategy gating on BBO must not read a stale touch as a live one.
[[nodiscard]] std::optional<Bbo>
map_bbo(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Reads relative_funding_rate, NOT funding_rate: only the relative field is
// dimensionless across the PF_/PI_ families (see wire.hpp). nullopt when no
// relative rate is quoted — ticker_lite carries none, and a contract without
// funding must not be reported as one funding at zero.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Builds a domain instrument from a GET /instruments row. nullopt for
// anything not tradeable, not the requested market, or whose steps failed to
// parse — an unusable tick size must never enter the table, because every
// later price conversion divides by it.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

} // namespace raikiri::venues::kraken
