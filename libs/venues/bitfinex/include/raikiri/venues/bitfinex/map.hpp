#pragma once

// Wire types -> domain types.
//
// SIZES ARE SIGNED AND THE SIGN IS NOT THE SIZE. A trade's AMOUNT encodes the
// aggressor and a book level's AMOUNT encodes the side; both must become a
// magnitude plus a flag rather than a negative quantity.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/bitfinex/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::bitfinex {

// DERIVED, NOT VENUE-STATED — and this is the one number in the adapter worth
// arguing about.
//
// Bitfinex publishes NO step anywhere. /conf/pub:info:pair gives
// MIN_ORDER_SIZE and MAX_ORDER_SIZE and nothing else numeric; there is no
// tick, no lot, no precision field, on any endpoint. The documented rule is
// FIVE SIGNIFICANT FIGURES, and significant figures are not a grid: the
// spacing they imply is 1.0 at 63947 and 0.00001 at 0.18767, so a tick
// derived from a price refuses that same instrument the moment it crosses a
// power of ten. (hyperliquid reached the same conclusion about the same rule.)
//
// What IS a fixed grid is the eight-decimal cap. Measured over every row of
// GET /tickers?symbols=ALL — 272 instruments, 1360 price tokens, 816 size
// tokens — no token carries more than 8 decimal places and no price exceeds 5
// significant figures. Where the two rules collide the decimal cap wins:
// tSHIB:USD quotes 0.00000449, three significant figures, because it cannot
// spell more. So 1e-8 is the finest spacing Bitfinex ever emits, it does not
// move with magnitude, and exact division by it has never yet dropped a value
// the venue actually sent.
//
// Uniform across every instrument, hence a constant rather than a per-row
// field. If Bitfinex ever states a real per-pair step, this is what it
// replaces.
inline constexpr std::int64_t k_step_e9 = 10; // 1e-8 in 1e9 fixed point

// Perpetual funding settles every eight hours. Verified rather than assumed:
// the `status` channel's NEXT_FUNDING_EVT_TIMESTAMP_MS landed on 08:00:00Z
// exactly, an integer multiple of 28800 s. Unlike okx there is only ONE
// settlement time in the payload, so no interval can be derived from a gap.
inline constexpr std::uint32_t k_funding_interval_s = 28800;

// Venue milliseconds -> UnixNanos. A zero or absent venue timestamp yields
// local ingest time: a zero propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// Exact decimal -> 1e9 fixed point for a BARE JSON NUMBER, scientific notation
// included. parse_decimal_e9 refuses an exponent outright and Bitfinex spells
// small amounts `1e-8` on the live trades channel, so the domain helper alone
// would drop those silently. int128 mantissa and a base-10 exponent: no double
// touches a price on the way in.
[[nodiscard]] std::optional<FixedE9> parse_number_e9(std::string_view text) noexcept;

// Public market data ------------------------------------------------------

// `symbol` is the venue symbol the channel id was bound to at subscribe time.
[[nodiscard]] std::optional<Bbo> map_bbo(const WireTicker& wire,
                                         std::string_view symbol,
                                         const exec::InstrumentTable& table,
                                         UnixNanos ts_event,
                                         UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick> map_trade(const WireTrade& wire,
                                                 std::string_view symbol,
                                                 const exec::InstrumentTable& table,
                                                 UnixNanos ts_init);

// Returns nullopt when the frame quotes no rate at all — a contract without
// funding must not be reported as one funding at zero.
[[nodiscard]] std::optional<FundingUpdate> map_funding(const WireDerivStatus& wire,
                                                       std::string_view symbol,
                                                       const exec::InstrumentTable& table,
                                                       UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Returns nullopt for a row that is not the requested market, for Bitfinex's
// paper-trading pairs, and for a row whose only number will not parse.
//
// The endpoint states no symbol prefix: rows read "BTCUSD" while every socket
// and REST path wants "tBTCUSD", so the prefix is added here rather than at
// each of the four call sites that would otherwise have to remember.
[[nodiscard]] std::optional<Instrument> map_instrument(const WirePairInfo& wire, MarketType market);

// Private -----------------------------------------------------------------
//
// Every signed field below is split into a Side plus a POSITIVE magnitude
// before it reaches the domain. That is the whole of this section's risk: a
// negative QtyLots is not caught anywhere downstream, and a fill booked to the
// wrong side moves the position by twice its size.

// What the caller already knows about an order the venue is now reporting a
// fill for. Bitfinex's execution rows carry no client order id — the docs
// place a CID at index 11 that its own model does not read — so the ULID
// arrives only out of the memo the caller keeps against the venue order id.
struct OrderContext {
    ExchangeClientOrderId client_order_id{};
    // NOT a default-constructed QtyLots: that is the UNDEF sentinel, and an
    // undefined leaves reaching shm is worse than the honest zero a fill for an
    // order nothing remembers can claim.
    QtyLots leaves{QtyLots::from_raw(0)};
};

// A lifecycle transition, or nullopt when the record carries none of its own.
// A fill is NOT one: the `te`/`tu` execution reports it, and announcing it
// again from the order record double-counts the volume.
[[nodiscard]] std::optional<OrderEvent> map_order_event(const WireOrder& wire,
                                                        const exec::InstrumentTable& table,
                                                        const ExchangeClientOrderId& client_id,
                                                        UnixNanos ts_init);

[[nodiscard]] std::optional<exec::OrderStatusReport>
map_open_order(const WireOrder& wire, const exec::InstrumentTable& table);

[[nodiscard]] std::optional<OrderEvent> map_execution(const WireExecution& wire,
                                                      const exec::InstrumentTable& table,
                                                      const OrderContext& context,
                                                      UnixNanos ts_init);

// Returns nullopt for a closed or flat row rather than a zero position:
// absent means flat, and a row Bitfinex keeps at amount 0 states nothing.
[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                               const exec::InstrumentTable& table);

} // namespace raikiri::venues::bitfinex
