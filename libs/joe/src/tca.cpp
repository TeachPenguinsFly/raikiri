#include <raikiri/joe/tca.hpp>

#include <algorithm>

namespace raikiri::joe {
namespace internal {

[[nodiscard]] std::uint64_t rank(const std::vector<std::uint64_t>& sorted, std::uint64_t pct) noexcept {
    return sorted.empty() ? 0 : sorted[(sorted.size() - 1) * pct / 100];
}

} // namespace internal

VenueRow& Tca::row_for(VenueId venue) {
    const auto it = std::ranges::find_if(rows_, [venue](const VenueRow& row) { return row.sum.venue == venue; });
    return it != rows_.end() ? *it : rows_.emplace_back(VenueRow{.sum = {.venue = venue}});
}

void Tca::add(const Parent& parent) {
    for (const auto& row : parent.retired()) {
        row_for(row.sum.venue).add(row);
    }
    const bool buy = parent.spec().side == Side::Buy;
    for (const auto& child : parent.children()) {
        row_for(child.instrument.venue()).add(child, parent.arrival(), buy);
    }
}

std::vector<VenueTca> Tca::venues() const {
    std::vector<VenueTca> out;
    out.reserve(rows_.size());
    for (const auto& row : rows_) {
        auto acks = row.acks;
        std::ranges::sort(acks);
        auto sum = row.sum;
        sum.ack_p50_ns = internal::rank(acks, 50);
        sum.ack_p90_ns = internal::rank(acks, 90);
        out.push_back(sum);
    }
    return out;
}

FixedE9 bp_of(FixedE9 part, FixedE9 base) noexcept {
    if (part.is_undef() || base.is_undef() || base.raw <= 0) {
        return FixedE9::undef();
    }
    return FixedE9::from_raw(mul_div(part.raw, 10'000 * k_e9, base.raw) / 1'000'000 * 1'000'000);
}

} // namespace raikiri::joe
