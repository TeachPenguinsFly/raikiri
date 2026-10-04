#pragma once

#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/binance/account.hpp>
#include <raikiri/venues/binance/book.hpp>
#include <raikiri/venues/binance/catalogue.hpp>
#include <raikiri/venues/binance/market_data.hpp>
#include <raikiri/venues/binance/orders.hpp>
#include <raikiri/venues/binance/positions.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::binance {

// a zero or absent venue time falls back to local ingest time
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBookTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WirePrint& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt when the frame quotes no funding rate
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireMarkPrice& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// binance states no turnover on the mark-price frame
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker24h& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

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

// a zero vol or an empty figure leaves that field undefined
[[nodiscard]] std::optional<OptionGreeks>
map_option_greeks(const WireOptionMark& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// the mark row's top of book, for an option whose book ticker has not spoken yet
[[nodiscard]] std::optional<Bbo>
map_option_quote(const WireOptionMark& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// a side with zero size is empty, not a level at that price
void blank_empty_sides(Bbo& bbo) noexcept;

// nullopt unless trading, in the requested market, with parseable filters
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market);

} // namespace raikiri::venues::binance
