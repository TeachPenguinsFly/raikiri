#pragma once

// compares as of the venue's stamp by rewinding fills booked after it; a break needs a second,
// independent pull

#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace raikiri::oms {

// kept only long enough to rewind it out of an earlier venue snapshot
struct BookedFill {
    // the venue's stamp, compared against the venue's boundary
    std::uint64_t ts_ns{0};
    // Signed the way the position moved: a buy adds, a sell subtracts.
    std::int64_t lots{0};
};

// What this process held at the instant the venue answered.
[[nodiscard]] std::int64_t
book_as_of(std::int64_t book_now, std::span<const BookedFill> booked, std::uint64_t boundary_ns) noexcept;

// drops fills older than any snapshot still worth comparing
void prune_fills(std::vector<BookedFill>& booked, UnixNanos now, std::uint64_t max_age_ns);

// Whether a disagreement has survived a second, independent snapshot.
class ReconGate {
public:
    // true once the break is seen at a second, different venue boundary
    [[nodiscard]] bool on_break(InstrumentId instrument, std::uint64_t boundary_ns);

    // clears the first disagreement but leaves the halt
    void on_agree(InstrumentId instrument);

    [[nodiscard]] bool halted(InstrumentId instrument) const noexcept;

    [[nodiscard]] std::size_t halted_count() const noexcept { return halted_.size(); }

private:
    std::unordered_map<std::uint64_t, std::uint64_t> first_break_;
    std::unordered_set<std::uint64_t> halted_;
};

} // namespace raikiri::oms
