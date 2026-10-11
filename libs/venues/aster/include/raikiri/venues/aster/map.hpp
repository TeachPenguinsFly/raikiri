#pragma once

#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/aster/account.hpp>
#include <raikiri/venues/aster/book.hpp>
#include <raikiri/venues/aster/catalogue.hpp>
#include <raikiri/venues/aster/market_data.hpp>
#include <raikiri/venues/aster/orders.hpp>
#include <raikiri/venues/aster/positions.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::aster {

// a zero or absent timestamp falls back to ingest time
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBookTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WirePrint& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt when the frame quotes no funding rate
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireMarkPrice& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// from /fapi/v1/ticker/24hr; the mark-price frame has no turnover
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker24h& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt for anything not trading, not the requested market, or with unparsed filters
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market);

// x alone decides the event; X also reports fills, so emitting on it would double-count
[[nodiscard]] std::optional<OrderEvent>
map_order_event(const WireOrderUpdate& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<exec::OrderStatusReport> map_open_order(const WireOrder& wire,
                                                                    const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::FillReport> map_user_trade(const WireTrade& wire, const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePositionRisk& wire,
                                                               const exec::InstrumentTable& table);

// drops rows with no asset or all zeros and keeps undef figures;
// multi_assets_margin must come from the same answer as the row
[[nodiscard]] std::optional<BalanceReport> map_balance(const WireAssetBalance& wire, bool multi_assets_margin);

} // namespace raikiri::venues::aster
