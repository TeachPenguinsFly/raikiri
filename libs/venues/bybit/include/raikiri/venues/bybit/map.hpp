#pragma once

#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/bybit/account.hpp>
#include <raikiri/venues/bybit/catalogue.hpp>
#include <raikiri/venues/bybit/market_data.hpp>
#include <raikiri/venues/bybit/orders.hpp>
#include <raikiri/venues/bybit/positions.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::bybit {

// a zero or absent timestamp falls back to ingest time
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

[[nodiscard]] Side side_from(std::string_view text) noexcept;

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt when a delta touches no funding field
[[nodiscard]] std::optional<FundingUpdate> map_funding(const WireTicker& wire,
                                                       const exec::InstrumentTable& table,
                                                       FundingMemo& memo,
                                                       UnixNanos ts_init);

// 24-hour turnover from the same frame; spot has it without funding
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt when the update carries no lifecycle transition
[[nodiscard]] std::optional<OrderEvent>
map_order_event(const WireOrderUpdate& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// a fill; always produces an event
[[nodiscard]] std::optional<OrderEvent>
map_execution(const WireExecution& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// the funding rows that map_execution refuses
[[nodiscard]] std::optional<LedgerEntry> map_funding_charge(const WireExecution& wire,
                                                            const exec::InstrumentTable& table,
                                                            UnixNanos ts_init);

[[nodiscard]] std::optional<exec::OrderStatusReport>
map_open_order(const WireOpenOrder& wire, const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                               const exec::InstrumentTable& table);

// nullopt when no coin is named; coin rows carry no time, so the caller passes one
[[nodiscard]] std::optional<BalanceReport> map_coin_balance(const WireCoinBalance& wire,
                                                            UnixNanos ts_venue);

// nullopt for anything not trading or with unparsed filters
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

} // namespace raikiri::venues::bybit
