#pragma once

// when a stop, a take-profit or a trailing stop fires; pure

#include <raikiri/joe/resting.hpp>

#include <span>

namespace raikiri::joe {

struct TriggerState {
    // it has seen a price; a trailing stop, one past its activation
    bool active{false};
    // a trailing stop's best price since then: the highest for a sell, the lowest for a buy
    FixedE9 mark{FixedE9::undef()};
    UnixNanos fired{};
    // the observation that fired it
    FixedE9 price{FixedE9::undef()};
};

// whether the trigger waits for the price to rise to its level: a buy stop, a sell take-profit, a buy trailing stop
[[nodiscard]] bool fires_up(const ParentSpec& spec) noexcept;

// undef while a trailing stop is not active
[[nodiscard]] FixedE9 trigger_level(const ParentSpec& spec, const TriggerState& state) noexcept;

// takes one price and says whether the trigger fires on it; a price already past the level fires, and a fired
// trigger takes nothing more
[[nodiscard]] bool observe(const ParentSpec& spec, TriggerState& state, FixedE9 price, UnixNanos now) noexcept;

// the best bid and ask among the venues whose own book is live, halved; undef when no venue's is. `venue` reads
// that venue alone
[[nodiscard]] FixedE9
trigger_mid(std::span<const VenueBook> bids, std::span<const VenueBook> asks, VenueId venue) noexcept;

} // namespace raikiri::joe
