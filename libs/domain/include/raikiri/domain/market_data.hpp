#pragma once

// Normalized market-data value types. Trivially copyable PODs: these are the
// payloads of shm channels and journal records. Every type carries the
// two-timestamp pair: ts_event (venue's clock) and ts_init (our ingest
// clock); latency accounting and staleness gates always use ts_init.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>

#include <array>
#include <cstdint>
#include <type_traits>

namespace raikiri {

// Best bid/offer: the datum strategies gate on every tick. Sized to stay
// inside one 64-byte cache line as a seqlock cell payload.
struct Bbo {
    InstrumentId instrument{};
    PriceTicks bid{};
    PriceTicks ask{};
    QtyLots bid_qty{};
    QtyLots ask_qty{};
    UnixNanos ts_event{};
    UnixNanos ts_init{};
};

static_assert(sizeof(Bbo) == 56, "Bbo + seqlock header must fit a 64B cache line");
static_assert(std::is_trivially_copyable_v<Bbo>);

struct TradeTick {
    InstrumentId instrument{};
    PriceTicks price{};
    QtyLots qty{};
    Side aggressor{Side::Buy};
    std::uint8_t pad_[7]{};
    UnixNanos ts_event{};
    UnixNanos ts_init{};
};

static_assert(std::is_trivially_copyable_v<TradeTick>);

// Top of book to a fixed depth, as a SNAPSHOT rather than a delta.
//
// The venue's sequencing rules — bybit's update_id+1, binance's pu chain,
// okx's seqId, bitfinex's per-channel ids — stay inside the adapter that
// understands them. What crosses into shared memory is the book the adapter
// currently believes in, so a consumer never has to replay a venue's deltas
// or recover from a gap it cannot see.
inline constexpr std::size_t k_book_levels = 100;

struct BookLevel {
    PriceTicks price{};
    QtyLots qty{};
};

struct BookSnapshot {
    InstrumentId instrument{};
    std::uint32_t bid_count{0}; // levels actually filled; the rest are stale
    std::uint32_t ask_count{0};
    UnixNanos ts_event{};
    UnixNanos ts_init{};
    std::uint64_t venue_seq{0}; // the venue's own ordering token, for forensics
    std::array<BookLevel, k_book_levels> bids{};
    std::array<BookLevel, k_book_levels> asks{};
};

struct FundingUpdate {
    InstrumentId instrument{};
    FixedE9 rate{};           // current interval rate
    FixedE9 predicted_rate{}; // venue-published next-interval estimate
    PriceTicks mark_price{};
    UnixNanos next_funding_time{};
    std::uint32_t interval_s{0};
    std::uint8_t pad_[4]{};
    UnixNanos ts_event{};
    UnixNanos ts_init{};
};

static_assert(std::is_trivially_copyable_v<FundingUpdate>);

// What the venue says about an option beyond its price.
//
// THE VENUE'S NUMBERS, NOT OURS. Computing greeks here would need a vol
// surface, a rate curve and a dividend assumption, and every one of those is a
// number we would then have to defend against the venue's own — the same
// reasoning that decided funding is credited only from the venue's charge.
//
// The implied vols and the underlying ride along because a delta without them
// has no provenance: it was computed AT a vol and a spot, and a reader that
// cannot see which is reading a number it cannot check.
struct OptionGreeks {
    InstrumentId instrument{};
    FixedE9 delta{};
    FixedE9 gamma{};
    FixedE9 vega{};
    FixedE9 theta{};
    FixedE9 rho{};
    // RATIOS, not percents. Deribit quotes 172.79 for 172.79 %, and the
    // conversion happens at the parse edge because every other rate in this
    // system is a ratio and one percent slipping through reads as a 100x vol.
    FixedE9 mark_iv{};
    FixedE9 bid_iv{};
    FixedE9 ask_iv{};
    // In the QUOTE currency, not in the option's ticks: the option's tick is
    // the premium's step — 0.0001 BTC on Deribit — and the underlying is a
    // dollar price, so dividing one by the other means nothing.
    FixedE9 underlying_price{};
    UnixNanos ts_event{};
    UnixNanos ts_init{};
};

static_assert(sizeof(OptionGreeks) == 96);
static_assert(std::is_trivially_copyable_v<OptionGreeks>);

} // namespace raikiri
