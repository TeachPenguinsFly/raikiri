#pragma once

// The perpetual·perpetual funding board: what two venues quoting the same
// symbol are worth against each other, and what disqualifies the pair.
//
// TWO NUMBERS, NEVER ONE. The funding edge and the price spread are different
// quantities with different risks, and adding them into a single headline is
// what makes a board rank a route it cannot actually take. A pair whose funding
// pays well while the two perpetuals sit ten basis points apart is not an
// arbitrage; it is a directional position wearing a coupon.
//
// THE INTERVAL IS NORMALISED BEFORE ANYTHING IS COMPARED. Venues fund on
// different clocks — hourly, four-hourly, eight-hourly — and their stated rates
// are per interval, so two raw rates side by side describe different lengths of
// time. A pair where either leg does not state its interval is REFUSED rather
// than assumed to be on the common one: a venue that will not say cannot be
// normalised, and guessing eight hours for an hourly venue understates it
// eightfold.
//
// GROSS EDGE ONLY. No fees, no depth, no slippage. What is executable depends
// on size, on the account's fee tier and on the book past the top level, none
// of which this sees. Publishing a net number here would be a promise made out
// of numbers it does not have.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

namespace raikiri::scan {

// One venue's word on one symbol, with prices already in quote units rather
// than ticks — two venues size their ticks differently, and a spread taken in
// ticks compares nothing.
struct Leg {
    VenueId venue{};
    InstrumentId instrument{};
    FixedE9 bid{};
    FixedE9 ask{};
    FixedE9 rate{};              // for THIS venue's interval, not normalised
    std::uint32_t interval_s{0}; // 0 means the venue did not say
    UnixNanos next_funding{};
    FixedE9 volume_24h_quote{};
};

enum class Reject : std::uint8_t {
    OneVenue,    // a venue against itself is not a route
    NoInterval,  // a leg that will not state its clock cannot be normalised
    NoQuote,     // no bid, no ask, or a price that is not positive
    ThinVolume,  // below the floor on at least one leg
    CrossedBook, // bid above ask on a leg: the venue contradicted itself
};

[[nodiscard]] constexpr std::string_view to_string(Reject why) noexcept {
    using enum Reject;
    switch (why) {
    case OneVenue:
        return "same venue";
    case NoInterval:
        return "a leg does not state its funding interval";
    case NoQuote:
        return "a leg has no usable quote";
    case ThinVolume:
        return "below the volume floor";
    case CrossedBook:
        return "a leg's book is crossed";
    }
    return "?";
}

struct Row {
    // ORIENTED SO THE FUNDING EDGE IS POSITIVE. `long_leg` is the one to hold
    // — it pays the least or is paid — and `short_leg` is the one to sell,
    // which charges the most. A board of negative edges is the same information
    // written backwards, and every reader has to flip it in their head.
    Leg long_leg{};
    Leg short_leg{};

    // The rate difference, both legs converted to what they accrue over eight
    // hours. Eight because most venues fund on it, so the number reads as a
    // rate a person already has a feel for.
    FixedE9 funding_edge_8h{};

    // Buy the long leg at its ASK, sell the short leg at its BID — the prices
    // actually available, never the mid. Positive means the route opens in
    // profit before funding; negative is the entry cost of getting on.
    std::int64_t price_spread_bp{0};

    // The thinner leg. A route is only as deep as the side that runs out first.
    FixedE9 volume_floor{};
};

// A leg's rate restated as what it accrues in one hour. Nullopt when the venue
// did not state an interval — the one input that cannot be defaulted.
[[nodiscard]] std::optional<FixedE9> per_hour(FixedE9 rate, std::uint32_t interval_s) noexcept;

// Prices one pair. The orientation is decided by the rates, not by the caller:
// whichever leg charges more is the one to be short of.
[[nodiscard]] std::expected<Row, Reject>
pair(const Leg& a, const Leg& b, FixedE9 min_volume_quote) noexcept;

} // namespace raikiri::scan
