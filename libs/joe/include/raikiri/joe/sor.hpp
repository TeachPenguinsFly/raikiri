#pragma once

#include <raikiri/joe/order.hpp>

#include <span>
#include <vector>

namespace raikiri::joe {

struct Level {
    FixedE9 price{};
    FixedE9 qty{};
};

// the side a taker would hit, best first
struct VenueBook {
    InstrumentId instrument{};
    FixedE9 taker_fee{FixedE9::from_raw(0)};
    FixedE9 lot{};
    FixedE9 min_notional{FixedE9::from_raw(0)};
    std::vector<Level> levels{};
    // what the venue's order entry offers a resting child
    bool amend{false};
    bool post_only{true};
};

struct Leg {
    InstrumentId instrument{};
    FixedE9 lot{};
    std::int64_t lots{0};
    // the deepest level the leg reaches, so an ioc there cannot walk further than planned
    FixedE9 limit{};
    FixedE9 qty{};
    FixedE9 value{};
    FixedE9 fee{};
};

struct Route {
    std::vector<Leg> legs{};
    // what no venue could take in whole lots within the limit
    FixedE9 unrouted{FixedE9::from_raw(0)};
};

// splits `qty` across venues by price after the taker fee, within the books and an optional
// limit; each leg is whole lots and clears its venue's minimum notional
[[nodiscard]] Route route(Side side, FixedE9 qty, FixedE9 limit, std::span<const VenueBook> books);

// the book with the best touch after the taker fee, null when none quotes; a resting child's limit
// is its own, so the parent's does not narrow this
// what one venue's book holds within `band_bp` of its own touch, by value
[[nodiscard]] FixedE9 depth_within(Side side, const VenueBook& book, FixedE9 band_bp) noexcept;

[[nodiscard]] const VenueBook* best_venue(Side side, std::span<const VenueBook> books, bool post_only) noexcept;

} // namespace raikiri::joe
