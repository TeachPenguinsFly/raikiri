#pragma once

// Normalized market-data value types. Trivially copyable PODs: these are the
// payloads of shm channels and journal records. Every type carries the
// two-timestamp pair: ts_event (venue's clock) and ts_init (our ingest
// clock); latency accounting and staleness gates always use ts_init.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>

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

struct FundingUpdate {
    InstrumentId instrument{};
    FixedE9 rate{};              // current interval rate
    FixedE9 predicted_rate{};    // venue-published next-interval estimate
    PriceTicks mark_price{};
    UnixNanos next_funding_time{};
    std::uint32_t interval_s{0};
    std::uint8_t pad_[4]{};
    UnixNanos ts_event{};
    UnixNanos ts_init{};
};

static_assert(std::is_trivially_copyable_v<FundingUpdate>);

} // namespace raikiri
