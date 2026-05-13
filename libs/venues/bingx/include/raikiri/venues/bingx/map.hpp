#pragma once

// Wire types -> domain types.
//
// The steps are DERIVED: BingX states pricePrecision and quantityPrecision —
// counts of decimal digits — and never a tick or lot size, so map_instrument
// builds 10^-precision itself where every other adapter copies a stated number.
//
// Sizes are BASE UNITS, not contracts, so contract_value is 1. VERIFIED
// arithmetically: BTC-USDT's volume 12916.89 against quoteVolume 835015259 at a
// last of 64161 reconciles only if the quantity is BTC.

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

// Venue milliseconds -> UnixNanos. A zero or absent venue timestamp yields
// local ingest time: a zero propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// 10^-precision as an exact decimal-e9 step, built as an integer power of ten
// because a double would not represent 1e-8 exactly and every later conversion
// divides by this. Refuses a precision past 9; the deepest live row is 8.
[[nodiscard]] std::optional<FixedE9> step_from_precision(std::int64_t precision) noexcept;

// Public market data ------------------------------------------------------

[[nodiscard]] std::optional<Bbo> map_bbo(const WireBookTicker& wire,
                                         const exec::InstrumentTable& table,
                                         MarketType market,
                                         UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick> map_trade(const WireTrade& wire,
                                                 const exec::InstrumentTable& table,
                                                 MarketType market,
                                                 UnixNanos ts_init);

// depth5 is a complete snapshot, so the mapper needs no book state: the newest
// frame replaces what shm holds. venue_seq stays zero because the channel
// carries no sequence number to record.
[[nodiscard]] std::optional<BookSnapshot> map_book(const WireDepth& wire,
                                                   const exec::InstrumentTable& table,
                                                   MarketType market,
                                                   UnixNanos ts_init);

// The top level of each side, for the swap, whose bookTicker is silent.
[[nodiscard]] std::optional<Bbo> touch_of(const BookSnapshot& book) noexcept;

// From the premiumIndex poll, not from a stream: BingX publishes no funding
// channel. Returns nullopt when the response quotes no rate at all — a
// contract without funding must not be reported as one funding at zero.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WirePremiumIndex& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// The 24-hour turnover off {sym}@ticker. nullopt when the frame states neither
// column and when the symbol does not resolve in THIS socket's market — the
// spot pair and the perpetual are spelled the same, so the caller's market is
// what tells them apart.
[[nodiscard]] std::optional<TickerStats> map_ticker_stats(const WireTicker& wire,
                                                          const exec::InstrumentTable& table,
                                                          MarketType market,
                                                          UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Returns nullopt for anything not online, anything that is not the requested
// market, and any row whose precisions yield no usable step.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market);

// Returns nullopt for anything not online, anything closed to API orders, and
// any row whose tick or step is finer than FixedE9 represents.
[[nodiscard]] std::optional<Instrument> map_spot_instrument(const WireSpotSymbol& wire);

// Private ------------------------------------------------------------------

// BingX spells a spot pair and a perpetual IDENTICALLY ("BTC-USDT" on both),
// so the by-symbol lookup answers with whichever market loaded first. Every
// private mapper therefore states the market it is reading and refuses a row
// the table holds under the other one: an order event attributed to the wrong
// market moves the wrong position.
[[nodiscard]] std::optional<exec::OrderStatusReport>
map_open_order(const WireOrder& wire, const exec::InstrumentTable& table, MarketType market);

// `fallback_client_id` is what the memo remembers for this order id: the SPOT
// push carries no client order id at all, so without it every spot fill would
// be unattributable.
[[nodiscard]] std::optional<OrderEvent>
map_order_update(const WireOrderUpdate& wire,
                 const exec::InstrumentTable& table,
                 MarketType market,
                 const ExchangeClientOrderId& fallback_client_id,
                 UnixNanos ts_init);

[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                               const exec::InstrumentTable& table);

// One settlement asset of the margin wallet, as one balance row. Nullopt when
// BingX named no asset: a balance without one is a number with no unit.
//
// `ts_venue` is the caller's, because this answer carries no stamp of its own —
// BingX states a timestamp on a REFUSAL and none on a success.
//
// Nothing is dropped for reading empty: this endpoint names the SETTLEMENT
// assets of the margin wallet and not every asset the venue would accept, so a
// zero here is BingX saying the wallet is empty rather than padding.
[[nodiscard]] std::optional<BalanceReport> map_balance(const WireBalance& wire, UnixNanos ts_venue);

} // namespace raikiri::venues::bingx
