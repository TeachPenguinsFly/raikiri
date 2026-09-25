#pragma once

// steps are derived from digit precisions; sizes are base units, so contract_value is 1

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/bingx/account.hpp>
#include <raikiri/venues/bingx/book.hpp>
#include <raikiri/venues/bingx/catalogue.hpp>
#include <raikiri/venues/bingx/market_data.hpp>
#include <raikiri/venues/bingx/orders.hpp>
#include <raikiri/venues/bingx/positions.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::bingx {

// zero or absent yields local ingest time
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// exact integer power of ten; refuses precision past 9
[[nodiscard]] std::optional<FixedE9> step_from_precision(std::int64_t precision) noexcept;

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBookTicker& wire, const exec::InstrumentTable& table, MarketType market, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, MarketType market, UnixNanos ts_init);

// a complete snapshot; venue_seq stays zero
[[nodiscard]] std::optional<BookSnapshot>
map_book(const WireDepth& wire, const exec::InstrumentTable& table, MarketType market, UnixNanos ts_init);

// top level of each side; the swap bookTicker is silent
[[nodiscard]] std::optional<Bbo> touch_of(const BookSnapshot& book) noexcept;

// from the premiumIndex poll; nullopt when no rate is quoted
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WirePremiumIndex& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt when neither column is set or the symbol is not in this market
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, MarketType market, UnixNanos ts_init);

// nullopt for rows not online, in another market, or without a usable step
[[nodiscard]] std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market);

// nullopt for rows not online, closed to api orders, or with a step finer than 1e-9
[[nodiscard]] std::optional<Instrument> map_spot_instrument(const WireSpotSymbol& wire);

// spot and perp share symbols, so each mapper names its market and refuses the other
[[nodiscard]] std::optional<exec::OrderStatusReport>
map_open_order(const WireOrder& wire, const exec::InstrumentTable& table, MarketType market);

// the spot push has no client id, so the memo's id is used
[[nodiscard]] std::optional<OrderEvent> map_order_update(const WireOrderUpdate& wire,
                                                         const exec::InstrumentTable& table,
                                                         MarketType market,
                                                         const ExchangeClientOrderId& fallback_client_id,
                                                         UnixNanos ts_init);

[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                               const exec::InstrumentTable& table);

// nullopt when no asset is named; ts_venue comes from the caller since the answer has no stamp
[[nodiscard]] std::optional<BalanceReport> map_balance(const WireBalance& wire, UnixNanos ts_venue);

} // namespace raikiri::venues::bingx
