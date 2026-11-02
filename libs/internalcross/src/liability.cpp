#include <raikiri/internalcross/domain/liability.hpp>

#include <algorithm>

namespace raikiri::internalcross {

std::vector<LiabilityRow> liability_rows(std::span<const portfolio::Position> positions,
                                         const std::function<BookKind(std::uint16_t)>& kind_of) {
    std::vector<LiabilityRow> rows;
    for (const auto& position : positions) {
        const auto lots = position.qty().raw;
        if (lots == 0) {
            continue;
        }
        const auto id = position.key().instrument;
        auto row = std::ranges::find_if(rows, [id](const LiabilityRow& r) { return r.instrument == id; });
        if (row == rows.end()) {
            rows.push_back({.instrument = id});
            row = std::prev(rows.end());
        }
        switch (kind_of(position.key().strategy_id)) {
        case BookKind::Client:
            (lots > 0 ? row->client_long : row->client_short) += lots > 0 ? lots : -lots;
            break;
        case BookKind::Hedge:
            row->hedge += lots;
            break;
        default:
            row->others += lots;
            break;
        }
    }
    return rows;
}

} // namespace raikiri::internalcross
