#pragma once

// a venue that cannot answer, a failed answer and an answer of zero stay distinct

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/position_report.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace raikiri::portfolio {

enum class ReconVerdict : std::uint8_t {
    Agree,   // both sides say the same quantity
    Break,   // they disagree; stop opening on this instrument and page a human
    Unknown, // the venue cannot or did not answer; not flat
};

[[nodiscard]] std::string_view to_string(ReconVerdict verdict) noexcept;

struct ReconResult {
    ReconVerdict verdict{ReconVerdict::Unknown};
    QtyLots local_qty{QtyLots::from_raw(0)};
    QtyLots venue_qty{QtyLots::from_raw(0)};
    std::string_view detail;
};

// `report` is nullopt when an answering venue's snapshot had no row (flat); `local` sums all
// strategies
[[nodiscard]] ReconResult
reconcile_position(bool venue_answered, const std::optional<PositionReport>& report, QtyLots local) noexcept;

} // namespace raikiri::portfolio
