#pragma once

// Wire types -> domain types.
//
// Quantities are counts of CONTRACTS, so contract_value carries what one is
// worth and cannot be pinned to 1.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/xt/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::xt {

// Venue milliseconds -> UnixNanos. A zero or absent venue timestamp yields
// local ingest time: a zero propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// A quantity step derived from a digit count: 10^-digits in 1e9 fixed point.
// Refuses a precision finer than the domain models, which is the same rule the
// tick size gets from parse_decimal_e9.
[[nodiscard]] std::optional<FixedE9> step_from_precision(std::int64_t digits) noexcept;

// Public market data ------------------------------------------------------

// Top of the snapshot. Returns nullopt when either side is empty, which is a
// real state on an illiquid contract and not a book with a zero-priced quote.
[[nodiscard]] std::optional<Bbo>
map_bbo(const WireDepth& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// The same snapshot, kept whole. `depth` is self-contained, so the mapper
// needs no book state: the newest frame replaces what shm holds.
[[nodiscard]] std::optional<BookSnapshot>
map_book(const WireDepth& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// mark_price is left undefined: XT ships mark on its own channel and funding
// by REST, so the two never arrive together and a zero here would read as a
// real price of zero.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireFundingRate& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Returns nullopt for anything not tradable through the API, anything that is
// not the requested market, and any row whose steps failed to parse.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

// The spot half. Refuses a pair that is not ONLINE, one the API may not trade,
// and one whose digit count is finer than FixedE9 models.
[[nodiscard]] std::optional<Instrument> map_spot_instrument(const WireSpotSymbol& wire);

// Private -----------------------------------------------------------------
//
// Every mapper below names the MARKET it is reading for, and refuses a row
// whose symbol resolves to the other one. XT spells a spot pair and a
// perpetual identically — both are `btc_usdt` — so one instrument table holds
// at most one of them, and without the check a spot fill would be booked
// against a perpetual's contract size.
//
// The private responses also echo symbols in UPPERCASE while the catalogue
// states them lowercase, so the lookup folds case before it asks.

// What the caller already knows about an order the venue is now reporting a
// fill for. XT's fill records are INCOMPLETE on both trees: the futures order
// push carries no clientOrderId, /order/trade-list carries neither a client id
// nor a SIDE, and the spot socket fill carries neither either. A fill whose
// direction is not stated and not remembered is dropped rather than booked to
// a guessed one — a fill on the wrong side moves the position by twice its size.
struct OrderContext {
    ExchangeClientOrderId client_order_id{};
    // NOT a default-constructed QtyLots: that is the UNDEF sentinel, and an
    // undefined leaves reaching shm is worse than the honest zero a fill for an
    // order nothing remembers can claim.
    QtyLots leaves{QtyLots::from_raw(0)};
    Side side{Side::Buy};
    bool side_known{false};
};

// A lifecycle transition, or nullopt when the record carries none of its own.
// A fill is NOT one: the trade record reports it, and re-announcing a fill
// from the order record double-counts the volume.
// `fallback_client_id` is what the caller remembers for this venue order id.
// The futures ORDER PUSH carries no clientOrderId at all — only the REST reads
// do — so without the memo a socket-driven lifecycle arrives unattributable.
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

// Returns nullopt for a flat row rather than a zero position: absent means
// flat, and a row XT keeps at size 0 is not evidence of anything.
[[nodiscard]] std::optional<exec::PositionReport>
map_futures_position(const WireFuturesPosition& wire, const exec::InstrumentTable& table);

[[nodiscard]] std::optional<OrderEvent> map_spot_order_event(const WireSpotOrder& wire,
                                                             const exec::InstrumentTable& table,
                                                             UnixNanos ts_init);

[[nodiscard]] std::optional<exec::OrderStatusReport>
map_spot_open_order(const WireSpotOrder& wire, const exec::InstrumentTable& table);

[[nodiscard]] std::optional<OrderEvent> map_spot_trade(const WireSpotTrade& wire,
                                                       const exec::InstrumentTable& table,
                                                       const OrderContext& context,
                                                       UnixNanos ts_init);

} // namespace raikiri::venues::xt
