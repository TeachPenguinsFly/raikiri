#include <raikiri/candles/deep.hpp>

#include <algorithm>

namespace raikiri::candles {

namespace {

[[nodiscard]] std::uint16_t lane_of(InstrumentId id) noexcept {
    return static_cast<std::uint16_t>((static_cast<unsigned>(id.venue()) << 8U) | static_cast<unsigned>(id.market()));
}

constexpr int k_tries = 2;

} // namespace

bool DeepBackfill::add(const Backfill::Gap& gap) {
    if (gap.to - gap.from <= k_slack) {
        return false;
    }
    auto [it, fresh] = lanes_.try_emplace(lane_of(gap.instrument));
    auto& lane = it->second;
    if (fresh) {
        lane.source = kline_venue(gap.instrument.venue(), gap.instrument.market());
    }
    if (lane.source == nullptr) {
        return false;
    }
    const auto size = lane.source->per_call();
    for (auto to = gap.to; to > gap.from; to -= size) {
        lane.parts.push_back(
            {.instrument = gap.instrument, .symbol = gap.symbol, .from = std::max(gap.from, to - size), .to = to});
    }
    return true;
}

std::vector<DeepBackfill::Due>
DeepBackfill::due(std::uint64_t mono_ns, const Backfill& klines, const FundingBackfill& funding) {
    std::vector<Due> out;
    for (auto& [_, lane] : lanes_) {
        if (lane.source == nullptr || lane.parts.empty() || mono_ns < lane.next_ns) {
            continue;
        }
        const auto& part = lane.parts.front();
        if (klines.busy(part.instrument) || funding.busy(part.instrument)) {
            continue;
        }
        out.push_back(
            {.part = part, .call = lane.source->call(part.symbol, part.from, part.to), .source = lane.source.get()});
        lane.parts.pop_front();
        lane.next_ns = mono_ns + 60'000'000'000ULL / std::max<std::uint32_t>(lane.source->per_minute(), 1);
    }
    return out;
}

void DeepBackfill::answered(const Due& due, std::size_t minutes) {
    if (minutes > 0) {
        return;
    }
    auto& parts = lanes_[lane_of(due.part.instrument)].parts;
    while (!parts.empty() && parts.front().instrument == due.part.instrument) {
        parts.pop_front();
    }
}

void DeepBackfill::retry(const Due& due) {
    auto& lane = lanes_[lane_of(due.part.instrument)];
    const auto key = due.part.instrument.packed ^
                     (static_cast<std::uint64_t>(static_cast<std::uint32_t>(due.part.from)) << 1U);
    if (++lane.tries[key] >= k_tries) {
        ++dropped_;
        return;
    }
    lane.parts.push_front(due.part);
}

void DeepBackfill::defer(const Due& due) {
    lanes_[lane_of(due.part.instrument)].parts.push_front(due.part);
}

std::vector<DeepBackfill::Load> DeepBackfill::load() const {
    std::vector<Load> out;
    for (const auto& [_, lane] : lanes_) {
        if (lane.source == nullptr || lane.parts.empty()) {
            continue;
        }
        const auto id = lane.parts.front().instrument;
        out.push_back({.venue = id.venue(),
                       .market = id.market(),
                       .calls = lane.parts.size(),
                       .per_minute = lane.source->per_minute()});
    }
    return out;
}

std::size_t DeepBackfill::queued() const noexcept {
    std::size_t n = 0;
    for (const auto& [_, lane] : lanes_) {
        n += lane.parts.size();
    }
    return n;
}

} // namespace raikiri::candles
