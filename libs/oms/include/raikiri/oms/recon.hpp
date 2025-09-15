#pragma once

// Comparing our book against the venue's answer, without comparing two
// different instants.
//
// THE VENUE ANSWERED IN THE PAST. Between the moment it read its own position
// and the moment we read its answer, our fills kept arriving — and every one
// of those is in our book and not in its. Subtracting the two as they stand
// reports a break on every busy instrument.
//
// So the comparison is made AS OF the venue's own stamp: each fill booked
// after that instant is rewound back out first. The boundary is the venue's
// clock and the rewound fills carry venue timestamps too, because a
// subtraction between two clocks means nothing.
//
// AND ONE DISAGREEMENT IS NOT A BREAK. A fill landing inside the round trip
// falls on the wrong side of the boundary by a hair and disagrees exactly
// once; a real break survives the next, independent pull. Halting on the first
// would freeze a busy instrument on network jitter — and the halt does not
// clear itself.

#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace raikiri::oms {

// One fill this process booked, kept only long enough to rewind it out of a
// comparison against a snapshot taken before the venue saw it.
struct BookedFill {
    // The VENUE's stamp, not ours — it is compared against the venue's own
    // boundary.
    std::uint64_t ts_ns{0};
    // Signed the way the position moved: a buy adds, a sell subtracts.
    std::int64_t lots{0};
};

// What this process held at the instant the venue answered.
[[nodiscard]] std::int64_t book_as_of(std::int64_t book_now,
                                      std::span<const BookedFill> booked,
                                      std::uint64_t boundary_ns) noexcept;

// Drops fills older than any snapshot still worth comparing against: past that
// window they can never be rewound past again, and keeping them only grows the
// vector.
void prune_fills(std::vector<BookedFill>& booked, UnixNanos now, std::uint64_t max_age_ns);

// Whether a disagreement has survived a second, independent snapshot.
class ReconGate {
public:
    // True the moment the break is CONFIRMED — seen at a DIFFERENT venue
    // boundary from the one that first disagreed. The same boundary twice is
    // the same answer read twice and confirms nothing.
    [[nodiscard]] bool on_break(InstrumentId instrument, std::uint64_t boundary_ns);

    // Agreement clears the memory of the first disagreement, but NOT the halt:
    // a halt is a decision a person unwinds, not one the next lucky pull does.
    void on_agree(InstrumentId instrument);

    [[nodiscard]] bool halted(InstrumentId instrument) const noexcept;

    [[nodiscard]] std::size_t halted_count() const noexcept { return halted_.size(); }

private:
    std::unordered_map<std::uint64_t, std::uint64_t> first_break_;
    std::unordered_set<std::uint64_t> halted_;
};

} // namespace raikiri::oms
