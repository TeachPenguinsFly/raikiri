#pragma once

// Wire types -> domain types.

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
map_trade(const WireAggTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// markPriceUpdate carries mark, index and funding together. Returns nullopt
// when the frame quotes no funding rate at all — a contract without funding
// must not be reported as one funding at zero.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireMarkPrice& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

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

// Catalogue ---------------------------------------------------------------

// Builds a domain instrument from an exchangeInfo row. Returns nullopt for
// anything not currently trading, anything that is not the requested market,
// and any row whose filters failed to parse — an instrument with an unusable
// tick size must never enter the table, because every later price conversion
// divides by it.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

} // namespace raikiri::venues::binance
