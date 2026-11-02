#pragma once

// child orders summed per venue over one parent or many; costs are in the quote asset

#include <raikiri/joe/parent.hpp>

#include <cstdint>
#include <vector>

namespace raikiri::joe {

class Tca {
public:
    void add(const Parent& parent);

    // venues in the order they were first seen
    [[nodiscard]] std::vector<VenueTca> venues() const;

private:
    [[nodiscard]] VenueRow& row_for(VenueId venue);

    std::vector<VenueRow> rows_;
};

// basis points of `base`, kept to three decimals; undef when there is no base
[[nodiscard]] FixedE9 bp_of(FixedE9 part, FixedE9 base) noexcept;

} // namespace raikiri::joe
