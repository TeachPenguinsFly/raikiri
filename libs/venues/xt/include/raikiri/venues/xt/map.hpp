#pragma once

// quantities are contract counts, so contract_value cannot be pinned to 1

#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/ledger_entry.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/xt/account.hpp>
#include <raikiri/venues/xt/book.hpp>
#include <raikiri/venues/xt/catalogue.hpp>
#include <raikiri/venues/xt/market_data.hpp>
#include <raikiri/venues/xt/orders.hpp>
#include <raikiri/venues/xt/positions.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::xt {

// a zero or absent timestamp falls back to ingest time
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// 10^-digits in 1e9 fixed point; refuses precision finer than the domain models
[[nodiscard]] std::optional<FixedE9> step_from_precision(std::int64_t digits) noexcept;

// nullopt when either side is empty, a real state on an illiquid contract
[[nodiscard]] std::optional<Bbo>
map_bbo(const WireDepth& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<BookSnapshot>
map_book(const WireDepth& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// the memo carries what the mark channel said, since the record is written whole
[[nodiscard]] std::optional<FundingUpdate> map_funding(const WireFundingRate& wire,
                                                       const exec::InstrumentTable& table,
                                                       FundingMemo& memo,
                                                       UnixNanos ts_init);

// merged with what the funding poll already said
[[nodiscard]] std::optional<FundingUpdate> map_mark_price(const WireMarkPrice& wire,
                                                          const exec::InstrumentTable& table,
                                                          FundingMemo& memo,
                                                          UnixNanos ts_init);

// futures fills only the quote column; its quantity is a contract count
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

[[nodiscard]] std::optional<Instrument> map_spot_instrument(const WireSpotSymbol& wire);

// mappers refuse a symbol that resolves to the other market (spot and perp share btc_usdt);
// private rows echo uppercase symbols, so the lookup folds case

// fills lack the client id or side on several surfaces; a fill with no known side is dropped
struct OrderContext {
    ExchangeClientOrderId client_order_id{};
    // explicit zero; the default is the undef sentinel
    QtyLots leaves{QtyLots::from_raw(0)};
    Side side{Side::Buy};
    bool side_known{false};
};

// lifecycle only; the futures order push has no clientOrderId, so fallback_client_id supplies it
[[nodiscard]] std::optional<OrderEvent>
map_futures_order_event(const WireFuturesOrder& wire,
                        const exec::InstrumentTable& table,
                        const ExchangeClientOrderId& fallback_client_id,
                        UnixNanos ts_init);

[[nodiscard]] std::optional<OrderEvent> map_futures_deal(const WireFuturesDeal& wire,
                                                         const exec::InstrumentTable& table,
                                                         const OrderContext& context,
                                                         UnixNanos ts_init);

[[nodiscard]] std::optional<exec::OrderStatusReport>
map_futures_open_order(const WireFuturesOrder& wire, const exec::InstrumentTable& table);

// nullopt for a flat row
[[nodiscard]] std::optional<exec::PositionReport>
map_futures_position(const WireFuturesPosition& wire, const exec::InstrumentTable& table);

// nullopt when xt names no coin
[[nodiscard]] std::optional<BalanceReport> map_futures_balance(const WireFuturesBalance& wire);

[[nodiscard]] std::optional<OrderEvent> map_spot_order_event(const WireSpotOrder& wire,
                                                             const exec::InstrumentTable& table,
                                                             UnixNanos ts_init);

[[nodiscard]] std::optional<exec::OrderStatusReport>
map_spot_open_order(const WireSpotOrder& wire, const exec::InstrumentTable& table);

[[nodiscard]] std::optional<OrderEvent> map_spot_trade(const WireSpotTrade& wire,
                                                       const exec::InstrumentTable& table,
                                                       const OrderContext& context,
                                                       UnixNanos ts_init);

// books funding and fees only and skips zero amounts; amount is already signed
[[nodiscard]] std::optional<LedgerEntry>
map_bill(const WireBill& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

} // namespace raikiri::venues::xt
