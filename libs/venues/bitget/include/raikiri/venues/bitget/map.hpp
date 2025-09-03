#pragma once

// Wire types -> domain types.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/bitget/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::bitget {

// Venue milliseconds -> UnixNanos. A zero or absent venue timestamp yields
// local ingest time: a zero propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// Public market data ------------------------------------------------------

// Both read the SAME `ticker` frame: top of book, mark and funding arrive in
// one element, so one subscription feeds two sinks.
[[nodiscard]] std::optional<Bbo>
map_bbo(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Returns nullopt when the frame quotes no rate at all — a contract without
// funding must not be reported as one funding at zero.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Snapshots only. books15 restates the whole top of book on every push, so the
// mapper needs no book state; an `update` off the incremental `books` channel
// lists changed levels alone and would have to be applied to one.
[[nodiscard]] std::optional<BookSnapshot>
map_book(const WireBooks& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] Side side_from(std::string_view text) noexcept;

// Private stream ----------------------------------------------------------

// One `orders` push becomes at most one event. A push caused by a trade
// carries the trade inline and becomes Filled; the status change it also
// reports is the one that fill implies, so reporting both would count the
// same trade twice. A status change with no trade attached becomes the
// lifecycle event, and anything else becomes nothing at all.
[[nodiscard]] std::optional<OrderEvent> map_order_update(const WireOrderUpdate& wire,
                                                         const exec::InstrumentTable& table,
                                                         UnixNanos ts_init);

// Reconciliation ----------------------------------------------------------

// Returns nullopt for an order that is no longer working: an open-order pull
// answers with what is open, and a filled row in it is a page boundary
// artefact, not something to reconcile against.
[[nodiscard]] std::optional<exec::OrderStatusReport>
map_open_order(const WireOrderUpdate& wire, const exec::InstrumentTable& table);

// Neither fills endpoint carries a clientOid, so the report joins back to an
// order by venue order id alone.
[[nodiscard]] std::optional<exec::FillReport> map_fill_report(const WireFill& wire,
                                                              const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                               const exec::InstrumentTable& table);

// Catalogue ---------------------------------------------------------------

// Returns nullopt for anything not tradable, anything that is not the
// requested market, and any row whose steps failed to parse.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market);

// The spot catalogue is a different endpoint with a different row shape, so
// it gets its own overload rather than a market argument: nothing in a spot
// row could produce a perp, and a shared entry point would have to pretend
// otherwise.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireSpotSymbol& wire);

} // namespace raikiri::venues::bitget
