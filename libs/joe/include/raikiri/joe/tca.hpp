#pragma once

// child orders summed per venue, per algo and per both over one parent or many; costs are in the quote asset

#include <raikiri/joe/parent.hpp>

#include <cstdint>
#include <vector>

namespace raikiri::joe {

// one algo's parents, on every venue or on one
struct AlgoTca {
    Algo algo{Algo::Twap};
    // invalid is every venue
    VenueId venue{VenueId::Invalid};
    std::uint32_t orders{0};
    std::uint32_t working{0};
    std::uint32_t failed{0};
    // their children; value is the filled notional
    VenueTca sum{};
    // what was asked at each parent's own price, so value over it is the fill rate
    FixedE9 asked{FixedE9::from_raw(0)};
    // as cost and benchmark, against the market's vwap over each parent's life; only parents that saw trades
    FixedE9 vwap_cost{FixedE9::from_raw(0)};
    FixedE9 vwap_benchmark{FixedE9::from_raw(0)};
};

class Tca {
public:
    void add(const Parent& parent);

    // venues in the order they were first seen
    [[nodiscard]] std::vector<VenueTca> venues() const;
    [[nodiscard]] std::vector<AlgoTca> algos() const;
    [[nodiscard]] std::vector<AlgoTca> venue_algos() const;

private:
    struct AlgoRow {
        AlgoTca sum{};
        VenueRow children{};
    };

    [[nodiscard]] VenueRow& row_for(VenueId venue);
    [[nodiscard]] static AlgoRow& row_for(std::vector<AlgoRow>& rows, Algo algo, VenueId venue);
    static void book(AlgoRow& into, const Parent& parent, const VenueRow& children);
    [[nodiscard]] static std::vector<AlgoTca> closed(const std::vector<AlgoRow>& rows);

    std::vector<VenueRow> rows_;
    std::vector<AlgoRow> algos_;
    std::vector<AlgoRow> cells_;
};

// a part of one, kept to four decimals; undef when there is no whole
[[nodiscard]] FixedE9 share_of(FixedE9 part, FixedE9 whole) noexcept;

// basis points of `base`, kept to three decimals; undef when there is no base
[[nodiscard]] FixedE9 bp_of(FixedE9 part, FixedE9 base) noexcept;

} // namespace raikiri::joe
