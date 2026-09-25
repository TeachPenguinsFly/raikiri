#pragma once

#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/coinbase/catalogue.hpp>
#include <raikiri/venues/coinbase/market_data.hpp>
#include <raikiri/venues/coinbase/orders.hpp>

#include <optional>

namespace raikiri::venues::coinbase {

// "2026-08-10T20:51:42.507746Z" -> UnixNanos; nullopt for any other shape
[[nodiscard]] std::optional<UnixNanos> parse_iso8601(std::string_view text) noexcept;

// a missing or unreadable timestamp falls back to ingest time
[[nodiscard]] UnixNanos iso_to_unix(std::string_view text, UnixNanos fallback) noexcept;

// the ticker is republished per match only, so quote moves between trades are invisible; ts_event is the match time
[[nodiscard]] std::optional<Bbo> map_bbo(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireMatch& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// 24-hour volume in base; the quote column stays zero
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt for any market but spot, anything not "online", trading_disabled, or unparsed increments
[[nodiscard]] std::optional<Instrument> map_instrument(const WireProduct& wire, MarketType market);

// size minus filled_size, both in base; coinbase publishes no leaves
[[nodiscard]] std::optional<QtyLots> order_leaves(const WireOrder& wire, const exec::InstrumentTable& table);

// client_id is passed in because the uuid cannot be turned back into the ulid; nullopt for done/filled rows
[[nodiscard]] std::optional<OrderEvent> map_order_event(const WireOrder& wire,
                                                        const exec::InstrumentTable& table,
                                                        const ExchangeClientOrderId& client_id,
                                                        UnixNanos ts_init);

[[nodiscard]] std::optional<exec::OrderStatusReport>
map_open_order(const WireOrder& wire, const exec::InstrumentTable& table, const ExchangeClientOrderId& client_id);

} // namespace raikiri::venues::coinbase
