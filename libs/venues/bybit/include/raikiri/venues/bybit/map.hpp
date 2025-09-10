#pragma once

// Wire types -> domain types.

#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/bybit/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::bybit {

// Venue milliseconds -> UnixNanos. A zero or absent venue timestamp yields
// the local ingest time: a fill with no time is still a fill, and a zero
// timestamp propagating into latency histograms would poison them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

[[nodiscard]] Side side_from(std::string_view text) noexcept;

// Public market data ------------------------------------------------------

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// tickers frames carry funding on linear perps. Returns nullopt when the
// frame is a delta that did not touch any funding field — an update that
// changes nothing must not overwrite what the strategy already has.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// The 24-hour turnover off the same frame. Separate from map_funding because
// spot has one and no funding, and because a delta that restates neither must
// publish neither.
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);


// Private stream ----------------------------------------------------------

// Lifecycle event for an order update, or nullopt when the update carries no
// lifecycle transition of its own (see the fills note above).
[[nodiscard]] std::optional<OrderEvent>
map_order_event(const WireOrderUpdate& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// A fill. Always produces an event: an execution IS the money moving.
[[nodiscard]] std::optional<OrderEvent>
map_execution(const WireExecution& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// The same topic's funding rows, which map_execution above refuses.
[[nodiscard]] std::optional<LedgerEntry> map_funding_charge(const WireExecution& wire,
                                                            const exec::InstrumentTable& table,
                                                            UnixNanos ts_init);

// Reconciliation ----------------------------------------------------------

[[nodiscard]] std::optional<exec::OrderStatusReport>
map_open_order(const WireOpenOrder& wire, const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                               const exec::InstrumentTable& table);

// One coin of the unified account, as one balance row. Nullopt when bybit
// named no coin: a balance without an asset is a number with no unit. The
// timestamp comes from the caller because the coin rows carry none of their
// own — the REST envelope's `time` is the only one there is.
[[nodiscard]] std::optional<BalanceReport> map_coin_balance(const WireCoinBalance& wire,
                                                            UnixNanos ts_venue);

// Catalogue ---------------------------------------------------------------

// Returns nullopt for anything not currently trading, and for entries whose
// filters failed to parse.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

} // namespace raikiri::venues::bybit
