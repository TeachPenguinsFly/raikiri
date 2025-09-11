#pragma once

// Wire types -> domain types.
//
// Decimals arrive as JSON numbers written by a float formatter, so "0.5",
// "1e-05" and "9.5e4" all appear on the wire and parse_decimal_e9 alone
// cannot read a single catalogue row.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/deribit/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::deribit {

// parse_decimal_e9 plus an exponent suffix, and exact only: a token needing
// more than nine fractional digits is refused, never rounded. Exposed because
// that refusal is worth pinning by test.
[[nodiscard]] std::optional<FixedE9> parse_number_e9(std::string_view token) noexcept;

// A zero or absent venue timestamp yields local ingest time: a zero reaching
// a latency histogram poisons it.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// Public market data ------------------------------------------------------

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Returns nullopt when the ticker quotes no funding at all: a dated future
// ships no funding fields, while a perpetual quoting exactly 0.0 was captured
// live, so absence and zero must not be read the same way.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// The venue's own option analytics off the same ticker. nullopt on anything
// that is not an option, which is what has_greeks() separates from an option
// whose greeks are genuinely zero.
[[nodiscard]] std::optional<OptionGreeks>
map_greeks(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// The 24-hour turnover off the same ticker's `stats`. Every kind on this
// channel carries it, options included, where `volume_usd` is the PREMIUM
// traded rather than the underlying's notional.
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Returns nullopt for anything inactive, linear, of another market, or whose
// steps failed to parse.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

// Order entry ------------------------------------------------------------

// What the order has LEFT: amount minus filled_amount, exact in lots. Deribit
// publishes no leaves field, and subtracting is only safe because both terms
// are denominated in the same unit lot_size divides by.
[[nodiscard]] std::optional<QtyLots> order_leaves(const WireOrder& wire,
                                                  const exec::InstrumentTable& table);

// The lifecycle event one order row reports, or nullopt when the row says
// nothing new — an "open" order that already has fills is described by its
// Filled events, and re-announcing it as Accepted is a transition the FSM
// rejects outright.
[[nodiscard]] std::optional<OrderEvent>
map_order_event(const WireOrder& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// One Filled event per execution. `leaves` is what the order has left AFTER
// this trade, which only the accompanying order row knows — and it must be a
// DEFINITE quantity: the OMS subtracts it from the order size, and an
// undefined one is refused here rather than turned into a zero that would
// close an order still working.
[[nodiscard]] std::optional<OrderEvent> map_fill(const WireUserTrade& wire,
                                                 const exec::InstrumentTable& table,
                                                 QtyLots leaves,
                                                 UnixNanos ts_init);

[[nodiscard]] std::optional<exec::OrderStatusReport>
map_open_order(const WireOrder& wire, const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::FillReport> map_fill_report(const WireUserTrade& wire,
                                                              const exec::InstrumentTable& table);

// A row whose size does not divide by the instrument's own step is dropped:
// a position reported to the reconciler off by a rounding is worse than one
// it knows it did not receive.
[[nodiscard]] std::optional<exec::PositionReport>
map_position(const WirePosition& wire, const exec::InstrumentTable& table, UnixNanos ts_venue);

} // namespace raikiri::venues::deribit
