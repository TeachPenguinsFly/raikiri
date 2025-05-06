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

} // namespace raikiri
