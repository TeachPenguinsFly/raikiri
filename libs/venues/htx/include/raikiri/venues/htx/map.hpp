#pragma once

// Wire types -> domain types.

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

// Venue milliseconds -> UnixNanos. A zero or absent venue timestamp yields
// local ingest time: a zero propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// Exact decimal-or-exponent token -> 1e9 fixed point. Refuses significance
// finer than 1e-9 rather than rounding it away: a tick flattened to zero makes
// every later price conversion a division by zero.
[[nodiscard]] std::optional<FixedE9> exact_e9(std::string_view token);

// The same reader, truncating below 1e-9 instead of refusing. Rates and
// turnover, never prices or sizes: HTX pads every funding rate to eighteen
// fractional digits with a tail that is not always zero, so an exact read
// would present a running venue as silence. What remains is eight significant
// figures of a number no order divides by.
[[nodiscard]] std::optional<FixedE9> rate_e9(std::string_view token);

// Public market data ------------------------------------------------------

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Returns nullopt when the frame quotes no rate at all — a contract without
// funding must not be reported as one funding at zero. mark_price stays
// undefined because this frame carries none. The interval comes from the
// instrument, which is where the catalogue put it.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireFunding& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// The 24-hour turnover off market.{code}.detail. nullopt when the tick states
// neither column and when the contract is not in the table. `vol` on the same
// tick is in CONTRACTS and is not read: see wire.hpp.
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireDetail& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// step0 is a whole book every frame, so no state is carried between them and
// there is no chain to break: the newest frame simply replaces what shm holds.
// venue_seq takes tick.mrid, the only field on this payload that orders frames.
[[nodiscard]] std::optional<BookSnapshot>
map_depth(const WireDepth& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Returns nullopt for anything not listed, anything that is not a USDT-M swap,
// and any row whose tick or contract size failed to parse.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market);

// Spot half of the catalogue. HTX states spot precision as DIGIT COUNTS, so
// the steps are 10^-n; a pair asking for more than nine fractional digits is
// refused rather than rounded to what FixedE9 can hold. `api-trading` is
// checked beside `state`: a pair can be online for the web and shut to keys.
[[nodiscard]] std::optional<Instrument> map_spot_symbol(const WireSpotSymbol& wire);

// Order entry ------------------------------------------------------------

struct OrderIdentity {
    ClientOrderId client_order_id{};
    ExchangeClientOrderId exchange_client_order_id{};
    ExchangeOrderId exchange_order_id{};
};

// HTX puts OUR id in a different field on each market — an int64 on the swap,
// a 64-character string on spot — so reading it back needs to know which.
// Getting it backwards leaves an order unroutable: OrderStore::route reads
// client_order_id first and exchange_client_order_id only after it.
//
// Returns nullopt for text that cannot be one of ours: an over-long spot id or
// a venue order id past what ExchangeOrderId holds.
[[nodiscard]] std::optional<OrderIdentity>
map_identity(std::string_view client_order_id, std::string_view order_id, MarketType market);

// The remaining size the order frame stated, in lots. Spot says it outright;
// the swap says how much filled, so this subtracts. Callers cache it per
// order: a fill frame on either surface can arrive without it, and the OMS
// treats leaves as authoritative.
[[nodiscard]] std::optional<QtyLots> order_leaves(const WireOrder& wire,
                                                  const exec::InstrumentTable& table);

// Lifecycle only. A frame that merely reports a fill yields nullopt: its
// Filled event comes from map_fill, which is the only place the exec id and
// the fee exist, and emitting from both would attribute one fill twice.
[[nodiscard]] std::optional<OrderEvent>
map_order_event(const WireOrder& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// One fill off a swap order push, which carries the trades inline. `leaves` is
// what the same frame's volume/trade_volume implies, so the caller does not
// need a memo on this surface.
[[nodiscard]] std::optional<OrderEvent> map_fill(const WireOrder& order,
                                                 const WireFill& fill,
                                                 const exec::InstrumentTable& table,
                                                 QtyLots leaves,
                                                 UnixNanos ts_init);

// One fill off spot's trade.clearing push or either surface's REST match rows.
// `leaves` comes from the caller's per-order cache when the frame states none,
// and `known_*` fill in the identity a match row does not carry.
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

// One settlement the venue charged, taken VERBATIM: `amount` is already signed
// the way the account moved.
//
// THE 30/31 SPLIT IS READ AS A CHECK, NOT AS THE SIGN. HTX states the
// direction twice — once in the type code and once in the amount — and states
// neither convention in prose, so a row where the two disagree is dropped
// rather than resolved by preferring one. Booking a disagreement either way
// puts a number in an append-only ledger that the venue's own books contradict.
//
// nullopt for any type but funding, for a contract outside the table, for a
// figure that will not read, and for that disagreement.
[[nodiscard]] std::optional<LedgerEntry> map_financial_record(const WireFinancialRecord& wire,
                                                              const exec::InstrumentTable& table,
                                                              UnixNanos ts_init);

} // namespace raikiri::venues::htx
