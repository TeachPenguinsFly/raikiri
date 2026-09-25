#include <raikiri/portfolio/reconcile.hpp>

namespace raikiri::portfolio {

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

ReconResult
reconcile_position(bool venue_answered, const std::optional<PositionReport>& report, QtyLots local) noexcept {
    ReconResult out{};
    out.local_qty = local;

    if (!venue_answered) {
        out.detail = "no position report from the venue";
        return out; // Unknown, and staying Unknown even when local is flat
    }
    if (!report.has_value()) {
        // no row in an arrived snapshot means flat
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
    out.detail = local.raw == 0 ? "venue holds a position we do not know about" : "quantities differ";
    return out;
}

} // namespace raikiri::portfolio
