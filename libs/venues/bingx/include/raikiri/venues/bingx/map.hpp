#pragma once

// Wire types -> domain types.
//
// Where BingX's vocabulary becomes ours: strings become instrument-scaled
// integers, symbols become interned ids. Everything above this file is
// venue-agnostic; everything below it is BingX-shaped.
//
// Two differences from okx are structural rather than cosmetic.
//
// The steps are DERIVED: BingX states pricePrecision and quantityPrecision —
// counts of decimal digits — and never a tick or lot size, so map_instrument
// builds 10^-precision itself where every other adapter copies a stated number.
//
// Sizes are BASE UNITS, not contracts, so contract_value is 1. VERIFIED
// arithmetically: BTC-USDT's volume 12916.89 against quoteVolume 835015259 at a
// last of 64161 reconciles only if the quantity is BTC.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/bingx/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::bingx {

// Venue milliseconds -> UnixNanos. A zero or absent venue timestamp yields
// local ingest time: a zero propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// 10^-precision as an exact decimal-e9 step, built as an integer power of ten
// because a double would not represent 1e-8 exactly and every later conversion
// divides by this. Refuses a precision past 9; the deepest live row is 8.
[[nodiscard]] std::optional<FixedE9> step_from_precision(std::int64_t precision) noexcept;

// Public market data ------------------------------------------------------

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBookTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// From the premiumIndex poll, not from a stream: BingX publishes no funding
// channel. Returns nullopt when the response quotes no rate at all — a
// contract without funding must not be reported as one funding at zero.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WirePremiumIndex& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Builds a domain instrument from a /quote/contracts row. Returns nullopt for
// anything not online, anything that is not the requested market, and any row
// whose precisions yield no usable step — every later price conversion divides
// by that step.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market);

} // namespace raikiri::venues::bingx
