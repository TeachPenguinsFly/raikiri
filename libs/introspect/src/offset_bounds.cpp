#include <raikiri/introspect/offset_bounds.hpp>

#include <algorithm>

namespace raikiri::introspect {

OffsetInterval intersect(OffsetInterval a, OffsetInterval b) noexcept {
    return {.lo = std::max(a.lo, b.lo), .hi = std::min(a.hi, b.hi)};
}

OffsetInterval sample_interval(const OffsetSample& sample) noexcept {
    const auto server = static_cast<std::int64_t>(sample.server_ns);
    return {.lo = server - static_cast<std::int64_t>(sample.received_ns),
            .hi = server + static_cast<std::int64_t>(sample.resolution_ns) - static_cast<std::int64_t>(sample.sent_ns)};
}

OffsetInterval rest_interval(const ClockEstimate& estimate) noexcept {
    if (estimate.method == ClockMethod::None) {
        return {};
    }
    const auto spread = static_cast<std::int64_t>(estimate.uncertainty_ns);
    return {.lo = estimate.offset_ns - spread, .hi = estimate.offset_ns + spread};
}

std::int64_t least_correction(OffsetInterval interval) noexcept {
    if (interval.empty()) {
        return 0;
    }
    if (interval.lo > 0) {
        return interval.lo;
    }
    if (interval.hi < 0) {
        return interval.hi;
    }
    return 0;
}

std::string_view to_string(OffsetSource source) noexcept {
    switch (source) {
    case OffsetSource::None:
        return "none";
    case OffsetSource::Rest:
        return "rest";
    case OffsetSource::Feed:
        return "feed";
    case OffsetSource::RestAndFeed:
        return "rest_and_feed";
    case OffsetSource::Conflict:
        return "conflict";
    }
    return "none";
}

CombinedOffset combine(const ClockEstimate& rest, std::optional<std::int64_t> feed_lo) noexcept {
    const bool probed = rest.method != ClockMethod::None;
    if (!probed && !feed_lo.has_value()) {
        return {};
    }
    CombinedOffset out;
    if (!probed) {
        out.source = OffsetSource::Feed;
        out.interval = {.lo = *feed_lo, .hi = k_open_above};
    } else {
        const auto probe = rest_interval(rest);
        out.source = OffsetSource::Rest;
        out.interval = probe;
        if (feed_lo.has_value()) {
            out.interval = intersect(probe, {.lo = *feed_lo, .hi = k_open_above});
            if (out.interval.empty()) {
                out.source = OffsetSource::Conflict;
                return out;
            }
            if (*feed_lo > probe.lo) {
                out.source = OffsetSource::RestAndFeed;
            }
        }
    }
    out.correction_ns = least_correction(out.interval);
    return out;
}

} // namespace raikiri::introspect
