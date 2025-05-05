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
#include <raikiri/exec/instrument_table.hpp>
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

// Catalogue ---------------------------------------------------------------

// Builds a domain instrument from a /contract/detail row. Returns nullopt for
// anything not enabled, for any market this endpoint does not serve, and for
// any row whose scales cannot become steps — an instrument with an unusable
// tick size must never enter the table, because every later price conversion
// divides by it.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

} // namespace raikiri::venues::mexc
