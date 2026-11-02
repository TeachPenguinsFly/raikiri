#pragma once

// the prices and sizes of a child that rests; pure

#include <raikiri/joe/sor.hpp>

#include <cstdint>
#include <span>

namespace raikiri::joe {

enum class BookState : std::uint8_t { Live, Stale, Crossed };

struct Quote {
    BookState state{BookState::Stale};
    FixedE9 bid{FixedE9::undef()};
    FixedE9 ask{FixedE9::undef()};
};

// one venue's touch; a side the market view left out is older than it will route on
[[nodiscard]] Quote
quote_of(InstrumentId instrument, std::span<const VenueBook> bids, std::span<const VenueBook> asks) noexcept;

// on the tick grid, rounded away from the other side, within the limit if there is one; a post-only
// child also stops a tick short of the other side's touch; undef when nothing positive is left
[[nodiscard]] FixedE9
rest_price(Side side, FixedE9 price, const Quote& quote, FixedE9 tick, FixedE9 limit, bool post_only) noexcept;

[[nodiscard]] FixedE9 peg_reference(Side side, PegTo to, const Quote& quote) noexcept;

[[nodiscard]] FixedE9
peg_price(Side side, const PegSpec& peg, const Quote& quote, FixedE9 tick, FixedE9 limit, bool post_only) noexcept;

// the next show in whole lots: at least one and never more than `left_lots`
[[nodiscard]] std::int64_t
next_show(const IcebergSpec& iceberg, FixedE9 lot, std::int64_t left_lots, std::uint64_t& random) noexcept;

// the lots a child may have on a venue: `want` lifted to the minimum notional and cut to the most one
// child may be worth; zero when those two cannot both hold or `left_lots` falls short of the minimum
[[nodiscard]] std::int64_t fit_lots(std::int64_t want,
                                    std::int64_t left_lots,
                                    FixedE9 lot,
                                    FixedE9 price,
                                    FixedE9 min_notional,
                                    FixedE9 max_notional) noexcept;

} // namespace raikiri::joe
