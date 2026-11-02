#include <raikiri/joe/trace.hpp>

#include <algorithm>

namespace raikiri::joe {

std::string_view to_string(RepriceWhy why) noexcept {
    switch (why) {
    case RepriceWhy::Place:
        return "place";
    case RepriceWhy::Refill:
        return "refill";
    case RepriceWhy::Peg:
        return "peg";
    case RepriceWhy::Closed:
        return "closed";
    case RepriceWhy::Rejected:
        return "rejected";
    case RepriceWhy::Stale:
        return "stale";
    case RepriceWhy::Crossed:
        return "crossed";
    }
    return "unknown";
}

void Trace::open(std::uint64_t interval_ns, std::size_t mids, std::size_t fills, std::size_t reprices) {
    interval_ns_ = interval_ns;
    mids_.open(mids);
    fills_.open(fills);
    reprices_.open(reprices);
}

bool Trace::due(UnixNanos now) const noexcept {
    if (mids_.capacity() == 0) {
        return false;
    }
    const auto* last = mids_.last();
    return last == nullptr || now.ns >= last->ts.ns + interval_ns_;
}

bool Trace::sample(UnixNanos ts, FixedE9 mid, FixedE9 vwap) noexcept {
    if (!due(ts) || mid.is_undef()) {
        return false;
    }
    mids_.push({.seq = ++seq_, .ts = ts, .mid = mid, .vwap = vwap});
    return true;
}

void Trace::fill(const FillMark& mark) noexcept {
    auto stamped = mark;
    stamped.seq = ++seq_;
    fills_.push(stamped);
}

void Trace::reprice(const RepriceMark& mark) noexcept {
    auto stamped = mark;
    stamped.seq = ++seq_;
    reprices_.push(stamped);
}

TraceShape trace_shape(std::uint64_t run_ns, std::uint64_t min_interval_ns, std::size_t max_mids) noexcept {
    if (max_mids < 2 || min_interval_ns == 0) {
        return {};
    }
    const auto interval = std::max<std::uint64_t>(min_interval_ns, (run_ns + max_mids - 2) / (max_mids - 1));
    const auto count = std::min<std::uint64_t>(max_mids, run_ns / interval + 2);
    return {.interval_ns = interval, .mids = static_cast<std::size_t>(count)};
}

} // namespace raikiri::joe
