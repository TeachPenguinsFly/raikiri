#pragma once

// Wire types -> domain types.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/okx/book.hpp>
#include <raikiri/venues/okx/catalogue.hpp>
#include <raikiri/venues/okx/market_data.hpp>
#include <raikiri/venues/okx/orders.hpp>
#include <raikiri/venues/okx/positions.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::okx {

// Venue milliseconds -> UnixNanos. A zero or absent venue timestamp yields
// local ingest time: a zero propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// Public market data ------------------------------------------------------

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Returns nullopt when the frame quotes no rate at all — a contract without
// funding must not be reported as one funding at zero. The memo carries what
// the mark-price channel has already said, because the record is written whole.
[[nodiscard]] std::optional<FundingUpdate> map_funding(const WireFundingRate& wire,
                                                       const exec::InstrumentTable& table,
                                                       FundingMemo& memo,
                                                       UnixNanos ts_init);

// The mark arrives on its own channel. Published as the same record, merged
// with whatever the funding-rate channel has already said.
[[nodiscard]] std::optional<FundingUpdate> map_mark_price(const WireMarkPrice& wire,
                                                          const exec::InstrumentTable& table,
                                                          FundingMemo& memo,
                                                          UnixNanos ts_init);

// The 24-hour turnover off the `tickers` channel, which is a different frame
// from the funding rate on this venue.
//
// Only ONE column is real on a derivative: OKX states the base asset in
// volCcy24h and the quote nowhere, so a SWAP row leaves volume_24h_quote at
// zero. vol24h is in CONTRACTS there and is deliberately not read — it
// compares with nothing. On SPOT the two names mean the other thing, and the
// mapper reads inst_type to tell them apart.
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] Side side_from(std::string_view text) noexcept;

// Private stream ----------------------------------------------------------

// One `orders` element is read TWICE, because it carries two independent
// facts. These two calls are the halves, and both must be made in this order
// for every element: a partially-filled order that OKX then expires arrives
// as one message, and emitting the cancel without the fill loses the money.

// The fill, or nullopt when the update carries no trade. A `state:"filled"`
// push with no tradeId is deliberately NOT a fill: OKX sends that as the
// terminal marker for a spot market order whose quantity already arrived on
// earlier tradeId-bearing pushes, and reading accFillSz off it counts the
// same fill twice.
[[nodiscard]] std::optional<OrderEvent>
map_order_fill(const WireOrder& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// The lifecycle transition, or nullopt when the update is only a fill.
[[nodiscard]] std::optional<OrderEvent>
map_order_event(const WireOrder& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Reconciliation ----------------------------------------------------------

[[nodiscard]] std::optional<exec::OrderStatusReport>
map_open_order(const WireOrder& wire, const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::FillReport> map_fill(const WireFill& wire,
                                                       const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                               const exec::InstrumentTable& table);

// Catalogue ---------------------------------------------------------------

// Returns nullopt for anything not live, anything that is not the requested
// market, and any row whose steps failed to parse.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

} // namespace raikiri::venues::okx
