#pragma once

// Wire types -> domain types.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/coinbase/catalogue.hpp>
#include <raikiri/venues/coinbase/market_data.hpp>
#include <raikiri/venues/coinbase/orders.hpp>

#include <optional>

namespace raikiri::venues::coinbase {

// "2026-08-10T20:51:42.507746Z" -> UnixNanos. Returns nullopt for anything
// that is not that shape; callers substitute ingest time rather than zero.
[[nodiscard]] std::optional<UnixNanos> parse_iso8601(std::string_view text) noexcept;

// A missing or unreadable venue timestamp yields local ingest time: a zero
// propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos iso_to_unix(std::string_view text, UnixNanos fallback) noexcept;

// Public market data ------------------------------------------------------

// The ticker channel is TRADE-DRIVEN: Coinbase republishes it once per match
// and never on a bare quote change (verified — 21 ticker frames over 20 s on
// BTC-USD joined one-to-one to 21 prints by trade_id). The BBO it carries is
// therefore a snapshot taken at each trade, and quote moves between trades
// are invisible on this feed. ts_event is the triggering match's time because
// that is the only time the frame states.
[[nodiscard]] std::optional<Bbo>
map_bbo(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireMatch& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// The 24-hour turnover off the same ticker frame, in the BASE asset. THE QUOTE
// COLUMN IS LEFT AT ZERO: Coinbase Exchange publishes no quote-side figure on
// this channel, and the ticker is trade-driven, so this republishes once per
// match rather than on a schedule.
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Returns nullopt for any market other than Spot, anything not "online",
// anything trading_disabled, and any row whose increments failed to parse.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireProduct& wire, MarketType market);

// Order entry ------------------------------------------------------------

// `size` minus `filled_size`, exact in lots. Coinbase publishes no leaves
// field and both terms are base currency, so the subtraction is the answer.
[[nodiscard]] std::optional<QtyLots> order_leaves(const WireOrder& wire,
                                                  const exec::InstrumentTable& table);

// The lifecycle event one order row reports.
//
// `client_id` is passed IN rather than read off the row: what comes back is
// the UUID derived from the ULID, and that derivation is one-way. The caller
// knows the ULID — from the command it just sent, or from the memo it kept —
// and an event carrying an empty one is an event nothing can route.
//
// Returns nullopt for a row that says nothing this adapter can honestly
// announce: an order Coinbase reports `done`/`filled` is described by its
// executions, and this adapter has no source for them (see fill_reports).
[[nodiscard]] std::optional<OrderEvent> map_order_event(const WireOrder& wire,
                                                        const exec::InstrumentTable& table,
                                                        const ExchangeClientOrderId& client_id,
                                                        UnixNanos ts_init);

[[nodiscard]] std::optional<exec::OrderStatusReport>
map_open_order(const WireOrder& wire,
               const exec::InstrumentTable& table,
               const ExchangeClientOrderId& client_id);

} // namespace raikiri::venues::coinbase
