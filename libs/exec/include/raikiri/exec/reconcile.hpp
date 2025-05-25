#pragma once

// Comparing what we think we hold against what the venue says.
//
// ABSENCE OF EVIDENCE IS NOT EVIDENCE OF FLATNESS. Most of this file exists to
// keep that one sentence true: a venue that cannot answer, an answer that
// failed, and an answer of zero are three different facts, and collapsing them
// produces the worst outcome available — concluding we are flat while holding
// a position.
//
// Deliberately a pure comparison. It performs no I/O, so the awkward part (what
// the venue said) is supplied by the caller and the decision is testable
// without a socket or a clock.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/exec/execution_client.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace raikiri::exec {

enum class ReconVerdict : std::uint8_t {
    Agree,   // both sides say the same quantity
    Break,   // they disagree; stop opening on this instrument and page a human
    Unknown, // the venue cannot or did not answer — NEVER treat as flat
};

[[nodiscard]] std::string_view to_string(ReconVerdict verdict) noexcept;

struct ReconResult {
    ReconVerdict verdict{ReconVerdict::Unknown};
    QtyLots local_qty{QtyLots::from_raw(0)};
    QtyLots venue_qty{QtyLots::from_raw(0)};
    std::string_view detail;
};

// `report` is nullopt when the pull was not performed or failed. `local` is the
// sum across strategies for this instrument: the venue nets, and has no idea
// the book is split by strategy.
//
// The venue's avg_entry is read for the record and never adopted. It is
// authoritative on whether a position exists and never on what it cost — it
// recomputes an average by its own rules, with or without leverage, with fees
// inside or outside, and two queries can disagree.
[[nodiscard]] ReconResult reconcile_position(bool venue_can_report,
                                             const std::optional<PositionReport>& report,
                                             QtyLots local) noexcept;

} // namespace raikiri::exec
