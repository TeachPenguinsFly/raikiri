#pragma once

// Wire types -> domain types.

#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/binance/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::binance {

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
// funding comes from: binance states no turnover on the mark-price frame.
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker24h& wire, const exec::InstrumentTable& table,
                 UnixNanos ts_init);

// Private stream ----------------------------------------------------------

// Lifecycle or fill for one order update, or nullopt when the frame reports
// no transition of ours.
//
// WHICH ASPECT OWNS THE EVENT — the rule that keeps positions from doubling.
// Binance packs both into one frame: `x` (execution type) says what happened,
// `X` (order status) says where the order now stands. A TRADE execution also
// reports X = PARTIALLY_FILLED or FILLED, so emitting an event for the status
// as well as for the execution counts the same fill twice.
//
// Therefore `x` alone decides:
//   NEW        -> Accepted      CANCELED  -> Canceled
//   TRADE      -> Filled        EXPIRED   -> Expired
//   AMENDMENT  -> Updated       CALCULATED-> nothing (liquidation bookkeeping)
// `X` is carried into reconciliation and never becomes an event of its own.
[[nodiscard]] std::optional<OrderEvent>
map_order_event(const WireOrderUpdate& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Reconciliation ----------------------------------------------------------

[[nodiscard]] std::optional<exec::OrderStatusReport>
map_open_order(const WireOrder& wire, const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::FillReport> map_user_trade(const WireTrade& wire,
                                                             const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePositionRisk& wire,
                                                               const exec::InstrumentTable& table);

// One margin asset, or nullopt for a row that reports no holding.
//
// Two rows are dropped, for different reasons. A row binance named no asset
// for is a number with no unit, and there is nothing to say about it. A row
// reporting zero everywhere is binance listing an asset it WOULD accept as
// collateral rather than one the account holds — the endpoint returns every
// such asset, and the shared-memory snapshot behind this holds 64 rows, so
// forwarding them buries the balances that exist under the ones that do not.
// A missing row and a zero row answer "how much do I hold" identically.
//
// A row where any figure is UNDEF is neither of those and is kept: not having
// been told is not the same as having been told zero.
[[nodiscard]] std::optional<BalanceReport> map_balance(const WireBalance& wire);

// Catalogue ---------------------------------------------------------------

// Returns nullopt for anything not currently trading, anything that is not the
// requested market, and any row whose filters failed to parse.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

} // namespace raikiri::venues::binance
