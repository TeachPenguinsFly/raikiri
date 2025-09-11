#pragma once

// Wire types -> domain types.
//
// The instrument's steps are DERIVED. MEXC states priceScale and volScale —
// counts of decimal digits — and never a tick or a lot size, so the step is
// built here as an integer power of ten. That derivation has a hard edge:
// FixedE9 carries nine fractional digits and MEXC lists contracts at scale 10
// and 11, which are refused rather than rounded.
//
// Funding rides on the ticker channel. MEXC's dedicated funding channel
// acknowledges a subscribe and then pushes nothing between settlements
// (verified: 90 s of silence), so the rate arrives with the ticker — which
// carries no settlement boundary, hence the separate REST overload.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/mexc/spot_wire.hpp>
#include <raikiri/venues/mexc/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::mexc {

// Venue milliseconds -> UnixNanos. A zero or absent venue timestamp yields
// local ingest time: a zero propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// 10^-scale as a FixedE9 step, built by integer division so no float ever
// holds a tick size. Returns nullopt past nine digits, which FixedE9 cannot
// represent.
[[nodiscard]] std::optional<FixedE9> step_from_scale(std::int64_t scale) noexcept;

// Public market data ------------------------------------------------------

// Refuses anything but a snapshot: an incremental push.depth frame lists only
// the levels that changed, in no particular order, so its first entry is not
// the touch.
[[nodiscard]] std::optional<Bbo>
map_bbo(const WireDepth& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Snapshots only, for the same reason: depth.full is self-contained, so the
// newest frame replaces what shm holds and the mapper carries no book state.
// Reconstructing a book from push.depth would mean holding one here.
[[nodiscard]] std::optional<BookSnapshot>
map_book(const WireDepth& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// The spot stream's own shapes. Separate from the futures mappers above
// because the two trees share no field, no encoding and no unit: futures
// quantities are CONTRACTS and spot's are base units, so a mapper reused
// across them would convert one of the markets with the other's lot size.
[[nodiscard]] std::optional<Bbo> map_spot_bbo(const StreamQuote& wire,
                                              std::string_view symbol,
                                              std::uint64_t ts_ms,
                                              const exec::InstrumentTable& table,
                                              UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick> map_spot_trade(const StreamTrade& wire,
                                                      std::string_view symbol,
                                                      const exec::InstrumentTable& table,
                                                      UnixNanos ts_init);

[[nodiscard]] std::optional<BookSnapshot> map_spot_book(const StreamBook& wire,
                                                        std::string_view symbol,
                                                        std::uint64_t ts_ms,
                                                        const exec::InstrumentTable& table,
                                                        UnixNanos ts_init);

// Returns nullopt when the frame quotes no rate at all — a contract without
// funding must not be reported as one funding at zero. next_funding_time and
// interval_s stay unset: the ticker frame states neither, and a zero interval
// reads as "unknown" where a guessed 8h would read as fact.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// The REST form, which does carry the schedule. Same output type so a caller
// that has both sources emits one kind of event.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireFundingRate& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// The 24-hour turnover off the same frame. QUOTE ONLY: the base column is left
// at zero because MEXC's `volume24` counts CONTRACTS, and a contract count put
// in a base-volume cell is a figure no other venue can be compared against.
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Returns nullopt for anything not enabled, for any market this endpoint does
// not serve, and for any row whose scales cannot become steps.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

// Spot catalogue. Refuses a row that is not trading, and one whose quantity
// step is finer than FixedE9 models.
[[nodiscard]] std::optional<Instrument> map_spot_instrument(const WireSpotSymbol& wire);

// Private -----------------------------------------------------------------

// MEXC's futures `side` is an OPEN/CLOSE enum, so the direction has to be
// recovered from it: 1 open long and 2 close short are both BUYS, 3 and 4 both
// SELLS. Reading it as a direction inverts every closing order.
[[nodiscard]] Side side_from_futures(std::int64_t side) noexcept;

// A lifecycle transition, or nullopt when the push carries none of its own.
// A working order with volume already dealt returns nullopt: the fill is the
// deal frame's to report, and re-announcing Accepted on every partial is a
// transition the FSM refuses.
[[nodiscard]] std::optional<OrderEvent> map_futures_order_event(const WireFuturesOrder& wire,
                                                                const exec::InstrumentTable& table,
                                                                UnixNanos ts_init);

// A fill. `leaves` is the caller's, because MEXC's deal frame does not state
// it — the order push is the only frame that does, and the OMS treats leaves
// as authoritative.
[[nodiscard]] std::optional<OrderEvent> map_futures_deal(const WireFuturesDeal& wire,
                                                         const exec::InstrumentTable& table,
                                                         QtyLots leaves,
                                                         const ExchangeClientOrderId& client_id,
                                                         UnixNanos ts_init);

[[nodiscard]] std::optional<exec::OrderStatusReport>
map_futures_open_order(const WireFuturesOrder& wire, const exec::InstrumentTable& table);

// Returns nullopt for a closed position rather than a zero one: absent means
// flat, and a row MEXC keeps at size 0 is not evidence of anything.
[[nodiscard]] std::optional<exec::PositionReport>
map_futures_position(const WireFuturesPosition& wire, const exec::InstrumentTable& table);

[[nodiscard]] std::optional<OrderEvent> map_spot_order_event(const WireSpotOrder& wire,
                                                             const exec::InstrumentTable& table,
                                                             UnixNanos ts_init);

[[nodiscard]] std::optional<exec::OrderStatusReport>
map_spot_open_order(const WireSpotOrder& wire, const exec::InstrumentTable& table);

// `is_buyer` is the ACCOUNT's side here, unlike the public trade channel's
// aggressor flag, and `leaves` again comes from the caller.
[[nodiscard]] std::optional<OrderEvent> map_spot_trade(const WireSpotTrade& wire,
                                                       const exec::InstrumentTable& table,
                                                       QtyLots leaves,
                                                       UnixNanos ts_init);

} // namespace raikiri::venues::mexc
