#include <raikiri/oms/recon.hpp>

#include <algorithm>

namespace raikiri::oms {

std::int64_t book_as_of(std::int64_t book_now,
                        std::span<const BookedFill> booked,
                        std::uint64_t boundary_ns) noexcept {
    for (const auto& fill : booked) {
        // strictly after; a fill at the boundary was already in the venue's answer
        if (fill.ts_ns > boundary_ns) {
            book_now -= fill.lots;
        }
    }
    return book_now;
}

void prune_fills(std::vector<BookedFill>& booked, UnixNanos now, std::uint64_t max_age_ns) {
    const auto cutoff = now.ns > max_age_ns ? now.ns - max_age_ns : 0;
    std::erase_if(booked, [cutoff](const BookedFill& fill) { return fill.ts_ns < cutoff; });
}

bool ReconGate::on_break(InstrumentId instrument, std::uint64_t boundary_ns) {
    const auto [it, fresh] = first_break_.try_emplace(instrument.packed, boundary_ns);
    if (fresh || it->second == boundary_ns) {
        return false; // the first disagreement, or the same snapshot read again
    }
    halted_.insert(instrument.packed);
    return true;
}

void ReconGate::on_agree(InstrumentId instrument) {
    first_break_.erase(instrument.packed);
}

bool ReconGate::halted(InstrumentId instrument) const noexcept {
    return halted_.contains(instrument.packed);
}

} // namespace raikiri::oms
