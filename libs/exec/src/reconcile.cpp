#include <raikiri/exec/reconcile.hpp>

namespace raikiri::exec {

std::string_view to_string(ReconVerdict verdict) noexcept {
    switch (verdict) {
    case ReconVerdict::Agree:
        return "agree";
    case ReconVerdict::Break:
        return "break";
    case ReconVerdict::Unknown:
        return "unknown";
    }
    return "unknown";
}

ReconResult reconcile_position(bool venue_can_report,
                               const std::optional<PositionReport>& report,
                               QtyLots local) noexcept {
    ReconResult out{};
    out.local_qty = local;

    if (!venue_can_report) {
        out.detail = "venue does not implement position reports";
        return out; // Unknown, and staying Unknown even when local is flat
    }
    if (!report.has_value()) {
        // No row came back for this instrument. On a venue that DOES report,
        // that is the venue saying flat — which is a real answer, unlike the
        // two cases above.
        out.venue_qty = QtyLots::from_raw(0);
        out.verdict = local.raw == 0 ? ReconVerdict::Agree : ReconVerdict::Break;
        out.detail = local.raw == 0 ? "both flat" : "venue reports flat, we hold a position";
        return out;
    }

    out.venue_qty = report->net_qty;
    if (report->net_qty.raw == local.raw) {
        out.verdict = ReconVerdict::Agree;
        out.detail = "quantities match";
        return out;
    }
    out.verdict = ReconVerdict::Break;
    out.detail = local.raw == 0 ? "venue holds a position we do not know about"
                                : "quantities differ";
    return out;
}

} // namespace raikiri::exec
