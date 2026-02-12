#pragma once

// Wire types -> domain types.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/aster/book.hpp>
#include <raikiri/venues/aster/catalogue.hpp>
#include <raikiri/venues/aster/market_data.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::aster {

// Venue milliseconds -> UnixNanos. A zero or absent venue timestamp yields
// local ingest time: a zero propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// Public market data ------------------------------------------------------

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBookTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WirePrint& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// markPriceUpdate carries mark, index and funding together. Returns nullopt
// when the frame quotes no funding rate at all — a contract without funding
// must not be reported as one funding at zero.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireMarkPrice& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// The 24-hour window off /fapi/v1/ticker/24hr. A separate read from the one
// funding comes from: aster states no turnover on the mark-price frame.
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker24h& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Returns nullopt for anything not currently trading, anything that is not the
// requested market, and any row whose filters failed to parse.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

} // namespace raikiri::venues::aster
