#pragma once

// Wire types -> domain types.
//
// Where KuCoin's vocabulary becomes ours: decimal text becomes
// instrument-scaled integers, symbols become interned ids. Everything above
// this file is venue-agnostic; everything below it is KuCoin-shaped.
//
// Three differences from the okx adapter are structural rather than cosmetic.
// Decimals can carry an exponent, because /api/v1/contracts/active serialises
// its steps with Java's default double formatting ("0.1" and "1.0E-10" both
// appear), so parse_decimal_e9 alone cannot read the catalogue. Sizes are
// CONTRACTS and `multiplier` converts to base units — negative on inverse
// contracts, which are refused rather than stored with a negative size. And
// the funding interval is stated by the catalogue rather than derived, since
// KuCoin runs 1h, 4h and 8h contracts side by side.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/kucoin/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::kucoin {

// Venue epochs -> UnixNanos. A zero or absent venue timestamp yields local
// ingest time: a zero propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;
[[nodiscard]] UnixNanos ns_to_unix(std::uint64_t ns, UnixNanos fallback) noexcept;

// parse_decimal_e9 plus an optional "E±n" suffix. Exposed because the
// catalogue's tick sizes reach 1.0E-10, one decade below what FixedE9 models,
// and the refusal of those rows is a property worth pinning by test.
[[nodiscard]] std::optional<FixedE9> parse_number_e9(std::string_view text) noexcept;

// Public market data ------------------------------------------------------

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// `symbol` overrides the DTO's own, because the REST form names the funding
// index rather than the contract. Returns nullopt when the frame quotes no
// rate at all — a contract without funding must not be reported as one
// funding at zero. mark_price is left undefined because KuCoin ships mark
// under a different subject; a zero there would read as a real price of zero.
[[nodiscard]] std::optional<FundingUpdate> map_funding(const WireFundingRate& wire,
                                                       std::string_view symbol,
                                                       const exec::InstrumentTable& table,
                                                       UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Builds a domain instrument from an /api/v1/contracts/active row. Returns
// nullopt for anything not Open, anything that is not a perpetual, anything
// inverse, and any row whose steps failed to parse — an instrument with an
// unusable tick size must never enter the table, because every later price
// conversion divides by it.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market);

} // namespace raikiri::venues::kucoin
