#pragma once

// ts_event is the venue's clock and ts_init ours; staleness gates use ts_init
// a zero ts_event means the venue did not stamp the message

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>

#include <array>
#include <cstdint>
#include <type_traits>

namespace raikiri {

// fits one 64-byte cache line as a seqlock payload
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

// a snapshot; venue sequencing stays in the adapter
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

// Whether `next_funding_time` of zero means "not said" or "no such instant".
enum class FundingAccrual : std::uint8_t {
    Unknown = 0,
    Discrete,   // paid at an instant
    Continuous, // accrues every moment; `interval_s` is the rate's unit
};

struct FundingUpdate {
    InstrumentId instrument{};
    FixedE9 rate{};           // current interval rate
    FixedE9 predicted_rate{}; // venue-published next-interval estimate
    PriceTicks mark_price{};
    UnixNanos next_funding_time{};
    std::uint32_t interval_s{0};
    FundingAccrual accrual{FundingAccrual::Unknown};
    std::uint8_t pad_[3]{};
    UnixNanos ts_event{};
    UnixNanos ts_init{};
};

static_assert(sizeof(FundingUpdate) == 64);
static_assert(std::is_trivially_copyable_v<FundingUpdate>);

// zero means the market did not trade; a missing cell means the venue said nothing
struct TickerStats {
    InstrumentId instrument{};
    FixedE9 volume_24h_base{};
    FixedE6 volume_24h_quote{};
    UnixNanos ts_event{};
    UnixNanos ts_init{};
};

static_assert(std::is_trivially_copyable_v<TickerStats>);

// the venue's own greeks, never computed here
struct OptionGreeks {
    InstrumentId instrument{};
    FixedE9 delta{};
    FixedE9 gamma{};
    FixedE9 vega{};
    FixedE9 theta{};
    FixedE9 rho{};
    // ratios, as raikiri/options/convention.hpp states
    FixedE9 mark_iv{};
    FixedE9 bid_iv{};
    FixedE9 ask_iv{};
    // in the quote currency
    FixedE9 underlying_price{};
    // the option's mark in OptionInstrument::premium units
    FixedE9 mark_price{};
    // in the quote currency
    FixedE9 index_price{};
    UnixNanos ts_event{};
    UnixNanos ts_init{};
};

static_assert(sizeof(OptionGreeks) == 112);
static_assert(std::is_trivially_copyable_v<OptionGreeks>);

} // namespace raikiri
