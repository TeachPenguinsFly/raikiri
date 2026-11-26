#include <raikiri/joe/tca.hpp>

#include <algorithm>

namespace raikiri::joe {
namespace internal {

[[nodiscard]] std::uint64_t rank(const std::vector<std::uint64_t>& sorted, std::uint64_t pct) noexcept {
    return sorted.empty() ? 0 : sorted[(sorted.size() - 1) * pct / 100];
}

[[nodiscard]] VenueTca closed(const VenueRow& row) {
    auto acks = row.acks;
    std::ranges::sort(acks);
    auto sum = row.sum;
    sum.ack_p50_ns = rank(acks, 50);
    sum.ack_p90_ns = rank(acks, 90);
    return sum;
}

} // namespace internal

VenueRow& Tca::row_for(VenueId venue) {
    const auto it = std::ranges::find_if(rows_, [venue](const VenueRow& row) { return row.sum.venue == venue; });
    return it != rows_.end() ? *it : rows_.emplace_back(VenueRow{.sum = {.venue = venue}});
}

Tca::AlgoRow& Tca::row_for(std::vector<AlgoRow>& rows, Algo algo, VenueId venue) {
    const auto it = std::ranges::find_if(
        rows, [&](const AlgoRow& row) { return row.sum.algo == algo && row.sum.venue == venue; });
    if (it != rows.end()) {
        return *it;
    }
    auto& made = rows.emplace_back();
    made.sum.algo = algo;
    made.sum.venue = venue;
    made.children.sum.venue = venue;
    return made;
}

void Tca::book(AlgoRow& into, const Parent& parent, const VenueRow& children) {
    auto& sum = into.sum;
    ++sum.orders;
    sum.working += parent.status() == Status::Working || parent.status() == Status::Paused ? 1U : 0U;
    sum.failed += parent.status() == Status::Failed ? 1U : 0U;
    into.children.add(children);
    const auto filled = children.sum.filled;
    const auto own = parent.avg_price().is_undef() ? parent.arrival() : parent.avg_price();
    if (!own.is_undef() && own.raw > 0) {
        // the parent's own size on every venue, what it sent on one
        const auto wanted = sum.venue == VenueId::Invalid ? parent.qty() : children.sum.sent;
        sum.asked = sum.asked + value_of(wanted, own);
    }
    if (const auto vwap = parent.market_vwap(); filled.raw > 0 && !vwap.is_undef() && vwap.raw > 0) {
        const auto at_vwap = value_of(filled, vwap);
        const auto value = children.sum.value;
        sum.vwap_benchmark = sum.vwap_benchmark + at_vwap;
        sum.vwap_cost = sum.vwap_cost + (parent.spec().side == Side::Buy ? value - at_vwap : at_vwap - value);
    }
}

void Tca::add(const Parent& parent) {
    std::vector<VenueRow> mine;
    const auto of = [&mine](VenueId venue) -> VenueRow& {
        const auto it = std::ranges::find_if(mine, [venue](const VenueRow& row) { return row.sum.venue == venue; });
        return it != mine.end() ? *it : mine.emplace_back(VenueRow{.sum = {.venue = venue}});
    };
    for (const auto& row : parent.retired()) {
        of(row.sum.venue).add(row);
    }
    const bool buy = parent.spec().side == Side::Buy;
    for (const auto& child : parent.children()) {
        of(child.instrument.venue()).add(child, parent.arrival(), buy);
    }
    VenueRow all{};
    for (const auto& row : mine) {
        row_for(row.sum.venue).add(row);
        book(row_for(cells_, parent.spec().algo, row.sum.venue), parent, row);
        all.add(row);
    }
    book(row_for(algos_, parent.spec().algo, VenueId::Invalid), parent, all);
}

std::vector<VenueTca> Tca::venues() const {
    std::vector<VenueTca> out;
    out.reserve(rows_.size());
    for (const auto& row : rows_) {
        out.push_back(internal::closed(row));
    }
    return out;
}

std::vector<AlgoTca> Tca::closed(const std::vector<AlgoRow>& rows) {
    std::vector<AlgoTca> out;
    out.reserve(rows.size());
    for (const auto& row : rows) {
        auto sum = row.sum;
        sum.sum = internal::closed(row.children);
        out.push_back(sum);
    }
    return out;
}

std::vector<AlgoTca> Tca::algos() const {
    return closed(algos_);
}

std::vector<AlgoTca> Tca::venue_algos() const {
    return closed(cells_);
}

FixedE9 share_of(FixedE9 part, FixedE9 whole) noexcept {
    if (part.is_undef() || whole.is_undef() || whole.raw <= 0) {
        return FixedE9::undef();
    }
    return FixedE9::from_raw(mul_div(part.raw, k_e9, whole.raw) / 100'000 * 100'000);
}

FixedE9 bp_of(FixedE9 part, FixedE9 base) noexcept {
    if (part.is_undef() || base.is_undef() || base.raw <= 0) {
        return FixedE9::undef();
    }
    return FixedE9::from_raw(mul_div(part.raw, 10'000 * k_e9, base.raw) / 1'000'000 * 1'000'000);
}

} // namespace raikiri::joe
