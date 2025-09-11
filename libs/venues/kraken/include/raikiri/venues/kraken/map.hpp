#pragma once

// Wire types -> domain types: numeric tokens become instrument-scaled
// integers, product ids become interned ids.
//
// Values arrive in two spellings, so parse_number_e9 replaces
// parse_decimal_e9 everywhere — the latter cannot read exponents at all.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/kraken/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::kraken {

// Venue ms -> UnixNanos. A zero or absent venue timestamp yields ingest time:
// a zero propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// Exact decimal-or-exponent token -> 1e9 fixed point. Reads both spellings
// Kraken uses for one field and refuses significance below 1e-9 rather than
// rounding it away. No double at any step.
[[nodiscard]] std::optional<FixedE9> parse_number_e9(std::string_view text) noexcept;

// contractValueTradePrecision -> quantity step, NEGATED: 4 means 0.0001,
// -3 means 1000. VERIFIED against live books — PF_PEPEUSD (-3) quotes sizes
// that are all multiples of 1000 and not 10000, PF_TURBOUSD (-2) multiples
// of 100 and not 1000. nullopt outside what 1e9 can hold.
[[nodiscard]] std::optional<FixedE9> step_from_precision(std::int64_t precision) noexcept;

// Public market data ------------------------------------------------------

// nullopt for a suspended market: its last quote is not tradeable, and a
// strategy gating on BBO must not read a stale touch as a live one.
[[nodiscard]] std::optional<Bbo>
map_bbo(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Reads relative_funding_rate, NOT funding_rate: only the relative field is
// dimensionless across the PF_/PI_ families (see wire.hpp). nullopt when no
// relative rate is quoted — ticker_lite carries none, and a contract without
// funding must not be reported as one funding at zero.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// The 24-hour turnover off the same frame. nullopt when the frame states
// neither column and when the product is not in the table.
//
// The base column is left at zero on the INVERSE family: Kraken's `volume`
// counts contracts there, and a contract of PI_ is one USD of notional, not a
// coin. Reading it as base ranks an inverse book against a linear one by a
// number measured in another unit.
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Nullopt for anything not tradeable, not the requested market, or whose steps
// failed to parse.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

// Reconciliation pulls ----------------------------------------------------

// A row carries `unfilledSize` and `filledSize` and never the original size,
// so the order quantity here is their SUM. Reading unfilledSize as the size
// turns a half-filled order into a smaller order that never filled.
[[nodiscard]] std::optional<exec::OrderStatusReport>
map_open_order(const WireOpenOrder& wire, const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::FillReport> map_rest_fill(const WireFill& wire,
                                                            const exec::InstrumentTable& table);

// `price` is a full-precision average entry that no tick size divides. The
// SIZE must still convert exactly — a position off by a lot is a position — so
// a rounded average is reported as no average at all rather than dropping the
// row and telling a reconciler we are flat.
[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                               const exec::InstrumentTable& table);

// Private stream ----------------------------------------------------------

// What one `open_orders` delta does to the order it names. Kraken states the
// cause in `reason` and nothing else in the frame does, so this is the whole
// decision.
// An EDIT is deliberately absent: Kraken's documented `reason` vocabulary has
// no value for one, so an amend is confirmed off the editorder response rather
// than guessed at here.
enum class DeltaEffect : std::uint8_t {
    Accepted,
    Triggered,
    Canceled,
    Rejected,
    // A frame the fills feed already accounts for, or a cause we cannot name.
    // Silence beats inventing a lifecycle event the OMS cannot undo.
    Silent,
};

[[nodiscard]] DeltaEffect effect_of(std::string_view reason, bool is_cancel) noexcept;

[[nodiscard]] std::optional<OrderEvent> map_ws_order(const WireWsOrder& wire,
                                                     OrderEventType type,
                                                     const exec::InstrumentTable& table,
                                                     UnixNanos ts_init);

[[nodiscard]] std::optional<OrderEvent>
map_ws_fill(const WireWsFill& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

} // namespace raikiri::venues::kraken
