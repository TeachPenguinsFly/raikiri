#pragma once

// decimals arrive in float formatting, exponents included

#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/deribit/book.hpp>
#include <raikiri/venues/deribit/catalogue.hpp>
#include <raikiri/venues/deribit/market_data.hpp>
#include <raikiri/venues/deribit/orders.hpp>
#include <raikiri/venues/deribit/positions.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::deribit {

// exact only: more than nine fractional digits is refused
[[nodiscard]] std::optional<FixedE9> parse_number_e9(std::string_view token) noexcept;

// zero or absent timestamps fall back to ingest time
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt when the ticker has no funding fields; a perpetual can quote exactly 0
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt on anything that is not an option
[[nodiscard]] std::optional<OptionGreeks>
map_greeks(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// on options volume_usd is the premium traded
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

// amount minus filled_amount; deribit publishes no leaves field
[[nodiscard]] std::optional<QtyLots> order_leaves(const WireOrder& wire,
                                                  const exec::InstrumentTable& table);

// nullopt when the row says nothing new; an open order with fills is described by its fills
[[nodiscard]] std::optional<OrderEvent>
map_order_event(const WireOrder& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// leaves must be definite since the oms subtracts it from the order size
[[nodiscard]] std::optional<OrderEvent> map_fill(const WireUserTrade& wire,
                                                 const exec::InstrumentTable& table,
                                                 QtyLots leaves,
                                                 UnixNanos ts_init);

[[nodiscard]] std::optional<exec::OrderStatusReport>
map_open_order(const WireOrder& wire, const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::FillReport> map_fill_report(const WireUserTrade& wire,
                                                              const exec::InstrumentTable& table);

// rows whose size is not a multiple of the step are dropped
[[nodiscard]] std::optional<exec::PositionReport>
map_position(const WirePosition& wire, const exec::InstrumentTable& table, UnixNanos ts_venue);

} // namespace raikiri::venues::deribit
