#pragma once

// Wire types -> domain types.
//
// The one place where bybit's vocabulary becomes ours: strings become
// instrument-scaled integers, symbols become interned ids, and venue statuses
// become OrderEvents. Everything above this file is venue-agnostic; everything
// below it is bybit-shaped. Keeping the boundary this sharp is what lets the
// eighth adapter be a week of work instead of a rewrite.
//
// The load-bearing decision here is WHICH STREAM OWNS FILLS.
//
// Bybit reports the same fill twice: the `execution` topic delivers it with an
// execId, price, quantity and fee, and the `order` topic separately reports the
// order's new cumulative quantity. Emitting a Filled event from both doubles
// every position in the system. So:
//
//   `execution` is authoritative for fills   — it alone produces Filled events
//   `order`     is authoritative for lifecycle — Accepted, Rejected, Canceled,
//                                                Expired, and nothing else
//
// An order update that merely reports "now Filled" therefore produces no event
// at all: the execution stream already delivered the fill that made it so.
// Reconciliation, not the order stream, is what catches a fill whose execution
// message was lost.

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

[[nodiscard]] std::optional<TradeTick> map_trade(const WireTrade& wire,
                                                 const exec::InstrumentTable& table,
                                                 UnixNanos ts_init);

// tickers frames carry funding on linear perps. Returns nullopt when the
// frame is a delta that did not touch any funding field — an update that
// changes nothing must not overwrite what the strategy already has.
[[nodiscard]] std::optional<FundingUpdate> map_funding(const WireTicker& wire,
                                                       const exec::InstrumentTable& table,
                                                       UnixNanos ts_init);

// Private stream ----------------------------------------------------------

// Lifecycle event for an order update, or nullopt when the update carries no
// lifecycle transition of its own (see the fills note above).
[[nodiscard]] std::optional<OrderEvent> map_order_event(const WireOrderUpdate& wire,
                                                        const exec::InstrumentTable& table,
                                                        UnixNanos ts_init);

// A fill. Always produces an event: an execution IS the money moving.
[[nodiscard]] std::optional<OrderEvent> map_execution(const WireExecution& wire,
                                                      const exec::InstrumentTable& table,
                                                      UnixNanos ts_init);

// Reconciliation ----------------------------------------------------------

[[nodiscard]] std::optional<exec::OrderStatusReport>
map_open_order(const WireOpenOrder& wire, const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                               const exec::InstrumentTable& table);

// Catalogue ---------------------------------------------------------------

// Builds a domain instrument from a catalogue entry. Returns nullopt for
// anything not currently trading, and for entries whose filters failed to
// parse — an instrument with an unusable tick size must never enter the
// table, because every later price conversion divides by it.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

} // namespace raikiri::venues::bybit
