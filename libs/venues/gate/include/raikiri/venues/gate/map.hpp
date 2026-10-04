#pragma once

// sizes are contracts: quanto_multiplier gives contract_value and the lot step is one contract

#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/gate/account.hpp>
#include <raikiri/venues/gate/book.hpp>
#include <raikiri/venues/gate/catalogue.hpp>
#include <raikiri/venues/gate/market_data.hpp>
#include <raikiri/venues/gate/orders.hpp>
#include <raikiri/venues/gate/positions.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace raikiri::venues::gate {

// a zero or absent venue time falls back to local ingest time
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// venue seconds, for the funding fields
[[nodiscard]] UnixNanos s_to_unix(std::uint64_t seconds) noexcept;

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBookTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// spot snapshot channel only; futures deltas go through DepthBook
[[nodiscard]] std::optional<BookSnapshot>
map_book(const WireBookUpdate& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt when the frame quotes no rate; mark price rides the same channel
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// the same, from a /contracts row
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireContract& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// volume_24h is in contracts and is not read
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt for non-trading, non-perp or unparsable rows
[[nodiscard]] std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market);

// BTC_USDT-20261225-100000-C is 26 bytes, past SymbolText, so an option is held as BTC-20261225-100000-C;
// nullopt for a name without the usdt underlying
[[nodiscard]] std::optional<SymbolText> option_symbol(std::string_view wire_name) noexcept;

// BTC-20261225-100000-C subscribes as BTC_USDT
[[nodiscard]] std::string option_underlying(std::string_view symbol);

// nullopt for an inactive row or a strike off the premium tick
[[nodiscard]] std::optional<Instrument> map_option_contract(const WireOptionContract& wire);

// a zero vol or an empty figure leaves that field undefined; ts_event has whole-second precision
[[nodiscard]] std::optional<OptionGreeks>
map_option_greeks(const WireOptionTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// the ticker's top of book, for a contract whose book ticker has not spoken yet
[[nodiscard]] std::optional<Bbo>
map_option_quote(const WireOptionTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// a side with zero size is empty, not a level at that price
void blank_empty_sides(Bbo& bbo) noexcept;

// steps are 10^-n; more than nine fractional digits is refused
[[nodiscard]] std::optional<Instrument> map_currency_pair(const WireCurrencyPair& wire);

// callers cache this per venue order id: a user-trade frame carries the fill but not the leaves
[[nodiscard]] std::optional<QtyLots> order_leaves(const WireOrder& wire, const exec::InstrumentTable& table);

// lifecycle only; fills come from the user-trade channel, which has the exec id and fee
[[nodiscard]] std::optional<OrderEvent>
map_order_event(const WireOrder& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// leaves and known_client_id come from the caller's per-order cache; a trade's own text wins
[[nodiscard]] std::optional<OrderEvent> map_user_trade(const WireUserTrade& wire,
                                                       const exec::InstrumentTable& table,
                                                       QtyLots leaves,
                                                       const ExchangeClientOrderId& known_client_id,
                                                       UnixNanos ts_init);

[[nodiscard]] std::optional<exec::OrderStatusReport> map_open_order(const WireOrder& wire,
                                                                    const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                               const exec::InstrumentTable& table);

// nullopt when gate named no currency
[[nodiscard]] std::optional<BalanceReport> map_futures_account(const WireFuturesAccount& wire);

} // namespace raikiri::venues::gate
