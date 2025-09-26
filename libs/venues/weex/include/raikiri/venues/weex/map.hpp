#pragma once

// Wire types -> domain types.

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/weex/catalogue.hpp>
#include <raikiri/venues/weex/market_data.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::weex {

// Venue milliseconds -> UnixNanos. A zero or absent venue timestamp yields
// local ingest time: a zero propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// Public market data ------------------------------------------------------

// The frame names the symbol once and carries several prints under it, so the
// symbol travels beside the print rather than inside it.
[[nodiscard]] std::optional<TradeTick> map_trade(const WirePrint& wire,
                                                 std::string_view symbol,
                                                 const exec::InstrumentTable& table,
                                                 UnixNanos ts_init);

// The 24-hour window off <sym>@ticker. Both columns or neither: a frame
// stating one of `v` and `q` and not the other leaves the missing one UNDEF,
// which is not the same claim as a market that turned over nothing.
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// One premiumIndex row. Returns nullopt when the row quotes no funding rate at
// all — a contract without funding must not be reported as one funding at zero,
// and WEEX does list contracts whose rate IS exactly zero (USDCUSDT was
// "0.000" live), so the two have to stay distinguishable.
//
// The rate is read with the TRUNCATING reader. WEEX pads a rate to eleven
// fractional digits on 349 of 1022 rows — "0.00000001988" live — and an exact
// read refuses every one of them, presenting a funding contract as silent.
//
// `indexPrice` rides along in the row and is deliberately NOT carried: it fails
// to divide by the contract's own tick on 446 of 1022 rows, so it cannot become
// a PriceTicks without being rounded into a number the venue never said.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WirePremium& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Returns nullopt for anything that is not the requested market, and for any
// row whose steps do not fit fixed point.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

} // namespace raikiri::venues::weex
