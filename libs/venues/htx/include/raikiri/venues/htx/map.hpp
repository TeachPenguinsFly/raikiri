#pragma once

#include <raikiri/domain/ledger_entry.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/htx/account.hpp>
#include <raikiri/venues/htx/book.hpp>
#include <raikiri/venues/htx/catalogue.hpp>
#include <raikiri/venues/htx/market_data.hpp>
#include <raikiri/venues/htx/orders.hpp>
#include <raikiri/venues/htx/positions.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::htx {

// a zero or absent timestamp falls back to ingest time
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// refuses precision finer than 1e-9 instead of rounding
[[nodiscard]] std::optional<FixedE9> exact_e9(std::string_view token);

// truncates below 1e-9; rates and turnover only, since htx pads funding rates to 18 digits
[[nodiscard]] std::optional<FixedE9> rate_e9(std::string_view token);

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt when the frame quotes no rate
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireFunding& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// vol on the detail tick is in contracts and is not read
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireDetail& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// venue_seq takes tick.mrid, the only field that orders frames
[[nodiscard]] std::optional<BookSnapshot>
map_depth(const WireDepth& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market);

// a pair needing more than nine fractional digits is refused
[[nodiscard]] std::optional<Instrument> map_spot_symbol(const WireSpotSymbol& wire);

struct OrderIdentity {
    ClientOrderId client_order_id{};
    ExchangeClientOrderId exchange_client_order_id{};
    ExchangeOrderId exchange_order_id{};
};

// our id is an int64 on swap and a string on spot; OrderStore::route reads client_order_id first
[[nodiscard]] std::optional<OrderIdentity>
map_identity(std::string_view client_order_id, std::string_view order_id, MarketType market);

// swap states filled and spot states remaining; callers cache it since fill frames may omit it
[[nodiscard]] std::optional<QtyLots> order_leaves(const WireOrder& wire,
                                                  const exec::InstrumentTable& table);

// lifecycle only; fills come from map_fill so one fill is not reported twice
[[nodiscard]] std::optional<OrderEvent>
map_order_event(const WireOrder& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<OrderEvent> map_fill(const WireOrder& order,
                                                 const WireFill& fill,
                                                 const exec::InstrumentTable& table,
                                                 QtyLots leaves,
                                                 UnixNanos ts_init);

// leaves falls back to the caller's cache; known_* supply the identity match rows lack
[[nodiscard]] std::optional<OrderEvent> map_user_trade(const WireUserTrade& wire,
                                                       const exec::InstrumentTable& table,
                                                       QtyLots leaves,
                                                       ClientOrderId known_client_id,
                                                       const ExchangeClientOrderId& known_ulid,
                                                       UnixNanos ts_init);

[[nodiscard]] std::optional<exec::OrderStatusReport>
map_open_order(const WireOrder& wire, const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                               const exec::InstrumentTable& table);

// amount is already signed; a row whose 30/31 type disagrees with the sign is dropped
[[nodiscard]] std::optional<LedgerEntry> map_financial_record(const WireFinancialRecord& wire,
                                                              const exec::InstrumentTable& table,
                                                              UnixNanos ts_init);

} // namespace raikiri::venues::htx
