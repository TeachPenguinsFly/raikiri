#pragma once

#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/okx/account.hpp>
#include <raikiri/venues/okx/book.hpp>
#include <raikiri/venues/okx/catalogue.hpp>
#include <raikiri/venues/okx/market_data.hpp>
#include <raikiri/venues/okx/orders.hpp>
#include <raikiri/venues/okx/positions.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::okx {

// zero or absent timestamps fall back to ingest time
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt when the frame quotes no rate; the memo merges in the mark-price channel
[[nodiscard]] std::optional<FundingUpdate> map_funding(const WireFundingRate& wire,
                                                       const exec::InstrumentTable& table,
                                                       FundingMemo& memo,
                                                       UnixNanos ts_init);

// merged with what the funding-rate channel already said
[[nodiscard]] std::optional<FundingUpdate> map_mark_price(const WireMarkPrice& wire,
                                                          const exec::InstrumentTable& table,
                                                          FundingMemo& memo,
                                                          UnixNanos ts_init);

// on derivatives volCcy24h is base and no quote is stated; vol24h counts contracts and is not read
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] Side side_from(std::string_view text) noexcept;

// each orders element is read twice, fill first and then lifecycle, so a fill before an expiry is kept

// a filled push without a tradeId is a terminal marker, not a fill
[[nodiscard]] std::optional<OrderEvent>
map_order_fill(const WireOrder& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<OrderEvent>
map_order_event(const WireOrder& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<exec::OrderStatusReport>
map_open_order(const WireOrder& wire, const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::FillReport> map_fill(const WireFill& wire,
                                                       const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                               const exec::InstrumentTable& table);

// nullopt without a currency; the account mode decides whether available is published
[[nodiscard]] std::optional<BalanceReport>
map_balance(const WireCurrencyBalance& wire, AccountMode mode, UnixNanos fallback);

[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

} // namespace raikiri::venues::okx
