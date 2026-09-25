#pragma once

#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/kucoin/account.hpp>
#include <raikiri/venues/kucoin/book.hpp>
#include <raikiri/venues/kucoin/catalogue.hpp>
#include <raikiri/venues/kucoin/market_data.hpp>
#include <raikiri/venues/kucoin/orders.hpp>
#include <raikiri/venues/kucoin/positions.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::kucoin {

// a zero or absent venue timestamp yields ingest time
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;
[[nodiscard]] UnixNanos ns_to_unix(std::uint64_t ns, UnixNanos fallback) noexcept;

// parse_decimal_e9 plus an optional E±n suffix; tick sizes reach 1.0E-10
[[nodiscard]] std::optional<FixedE9> parse_number_e9(std::string_view text) noexcept;

[[nodiscard]] std::optional<Bbo> map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// whole spot book; futures level2 changes go through DepthBook
[[nodiscard]] std::optional<BookSnapshot>
map_book(const WireBookSnapshot& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// futures only; spot instruments get no volume
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireContract& wire, const exec::InstrumentTable& table, UnixNanos ts_init);
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireSpotTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// symbol overrides the dto's since rest names the funding index; nullopt when no rate is quoted
[[nodiscard]] std::optional<FundingUpdate> map_funding(const WireFundingRate& wire,
                                                       std::string_view symbol,
                                                       const exec::InstrumentTable& table,
                                                       FundingMemo& memo,
                                                       UnixNanos ts_init);

// the mark arrives on its own subject and is merged with the funding memo
[[nodiscard]] std::optional<FundingUpdate>
map_mark_price(const WireMarkPrice& wire, const exec::InstrumentTable& table, FundingMemo& memo, UnixNanos ts_init);

// a filled notice yields nullopt; its fills already arrived as match
[[nodiscard]] std::optional<OrderEvent>
map_order_change(const WireOrderChange& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<exec::OrderStatusReport> map_open_order(const WireOrder& wire,
                                                                    const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::FillReport> map_fill(const WireFill& wire, const exec::InstrumentTable& table);

// avg_entry kept only when it lands on a tick
[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                               const exec::InstrumentTable& table);

// total is marginBalance, which excludes unrealised pnl; ts_init stands in for the missing venue time
[[nodiscard]] std::optional<BalanceReport> map_account_overview(const WireAccountOverview& wire, UnixNanos ts_init);

// nullopt unless open, perpetual, linear and its steps parse
[[nodiscard]] std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market);

// the spot catalogue states its steps as increments
[[nodiscard]] std::optional<Instrument> map_spot_symbol(const WireSpotSymbol& wire);

} // namespace raikiri::venues::kucoin
