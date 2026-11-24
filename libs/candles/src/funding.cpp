#include <raikiri/candles/funding.hpp>

#include <algorithm>

namespace raikiri::candles {

namespace {

[[nodiscard]] std::uint16_t lane_of(InstrumentId id) noexcept {
    return static_cast<std::uint16_t>((static_cast<unsigned>(id.venue()) << 8U) | static_cast<unsigned>(id.market()));
}

constexpr int k_tries = 2;

} // namespace

bool FundingBackfill::add(const Part& part) {
    if (part.to <= part.from) {
        return false;
    }
    auto [it, fresh] = lanes_.try_emplace(lane_of(part.instrument));
    auto& lane = it->second;
    if (fresh) {
        lane.source = funding_venue(part.instrument.venue(), part.instrument.market());
    }
    if (lane.source == nullptr) {
        return false;
    }
    lane.parts.push_back(part);
    return true;
}

std::vector<FundingBackfill::Due> FundingBackfill::due(std::uint64_t mono_ns, const Backfill& klines) {
    std::vector<Due> out;
    for (auto& [_, lane] : lanes_) {
        if (lane.source == nullptr || lane.parts.empty() || mono_ns < lane.next_ns) {
            continue;
        }
        const auto& part = lane.parts.front();
        if (klines.busy(part.instrument)) {
            continue;
        }
        out.push_back({.part = part,
                       .call = lane.source->call(part.symbol, part.from, part.to, part.page),
                       .source = lane.source.get()});
        lane.parts.pop_front();
        lane.next_ns = mono_ns + 60'000'000'000ULL / std::max<std::uint32_t>(lane.source->per_minute(), 1);
    }
    return out;
}

void FundingBackfill::answered(const Due& due, std::span<const MetricSample> rows) {
    if (rows.empty() || rows.size() < due.source->per_call()) {
        return;
    }
    const auto [oldest, newest] = std::ranges::minmax_element(rows, {}, [](const MetricSample& s) { return s.minute; });
    auto rest = due.part;
    rest.tries = 0;
    switch (due.source->more()) {
    case FundingVenue::More::None:
        return;
    case FundingVenue::More::Older:
        // an answer that does not move the window's end would be asked for ever
        if (oldest->minute >= rest.to) {
            return;
        }
        rest.to = oldest->minute;
        break;
    case FundingVenue::More::Newer:
        if (newest->minute < rest.from) {
            return;
        }
        rest.from = newest->minute + 1;
        break;
    case FundingVenue::More::Page:
        if (oldest->minute < rest.from) {
            return;
        }
        ++rest.page;
        break;
    }
    if (rest.to > rest.from) {
        lanes_[lane_of(rest.instrument)].parts.push_front(rest);
    }
}

void FundingBackfill::retry(const Due& due) {
    auto part = due.part;
    if (++part.tries >= k_tries) {
        ++dropped_;
        return;
    }
    lanes_[lane_of(part.instrument)].parts.push_front(part);
}

void FundingBackfill::defer(const Due& due) {
    lanes_[lane_of(due.part.instrument)].parts.push_front(due.part);
}

std::size_t FundingBackfill::queued() const noexcept {
    std::size_t n = 0;
    for (const auto& [_, lane] : lanes_) {
        n += lane.parts.size();
    }
    return n;
}

} // namespace raikiri::candles
