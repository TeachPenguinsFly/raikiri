#pragma once

// Wire types -> domain types.

#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/ledger_entry.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/weex/account.hpp>
#include <raikiri/venues/weex/catalogue.hpp>
#include <raikiri/venues/weex/market_data.hpp>
#include <raikiri/venues/weex/orders.hpp>
#include <raikiri/venues/weex/positions.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

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

// Private ------------------------------------------------------------------
//
// NONE OF THESE HAS RUN AGAINST A REAL BODY. There are no WEEX credentials on
// this machine, so every one below was written against the venue's documented
// shapes and is exercised only by the fixtures that copy them.

[[nodiscard]] Side side_from(std::string_view text) noexcept;

// One row of the open-order list, or the single-order read.
//
// LEAVES IS A SUBTRACTION, not a field: WEEX publishes `origQty` and a
// CUMULATIVE `executedQty` and nothing between them, and its status vocabulary
// has no PARTIALLY_FILLED at all — a half-filled order is still "NEW". A
// quantity that does not divide by the contract's own step leaves `leaves`
// UNDEF rather than zero, because zero is the claim that nothing is working.
[[nodiscard]] std::optional<exec::OrderStatusReport>
map_open_order(const WireOrder& wire, const exec::InstrumentTable& table);

// One row of the fill list. No client order id rides one, so the report names
// only the venue's own order id — an empty ULID here is the venue declining to
// say, never an id this adapter guessed.
[[nodiscard]] std::optional<exec::FillReport> map_fill(const WireFill& wire,
                                                       const exec::InstrumentTable& table);

// One open position. `avg_entry` stays UNDEF: WEEX states no entry price, and
// openValue over size is a quotient rather than a price it ever quoted.
[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                               const exec::InstrumentTable& table);

// The whole answer, NETTED PER INSTRUMENT. Under `separatedMode` SEPARATED a
// contract carries one row per direction, and everything downstream keys a
// position on the instrument alone — two rows published raw become whichever
// arrived last, reporting a hedged book as one naked leg. The timestamp kept is
// the latest of the rows merged.
[[nodiscard]] std::vector<exec::PositionReport> map_positions(std::span<const WirePosition> rows,
                                                              const exec::InstrumentTable& table);

// One collateral coin. Nullopt when WEEX named no asset: a balance without an
// asset is a number with no unit. The timestamp comes from the caller because
// the rows carry none of their own.
[[nodiscard]] std::optional<BalanceReport> map_balance(const WireBalance& wire, UnixNanos ts_venue);

// One funding settlement out of the account's bill list. Nullopt for a row of
// any other type, for a contract this gateway does not carry, and for an amount
// that will not parse — an unreadable figure is not a charge of zero.
[[nodiscard]] std::optional<LedgerEntry>
map_funding_charge(const WireBill& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Returns nullopt for anything that is not the requested market, and for any
// row whose steps do not fit fixed point.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

} // namespace raikiri::venues::weex
