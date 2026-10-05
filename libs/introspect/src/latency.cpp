#include <raikiri/introspect/latency.hpp>
#include <raikiri/introspect/offset_bounds.hpp>

#include <algorithm>

namespace raikiri::introspect {

std::string_view to_string(ClockMethod method) noexcept {
    switch (method) {
    case ClockMethod::None:
        return "none";
    case ClockMethod::RestProbe:
        return "rest_probe";
    }
    return "none";
}

std::string_view to_string(Stage stage) noexcept {
    switch (stage) {
    case Stage::Feed:
        return "feed";
    case Stage::Internal:
        return "internal";
    case Stage::KernelQueue:
        return "kernel_queue";
    case Stage::Rest:
        return "rest";
    case Stage::WsPing:
        return "ws_ping";
    case Stage::OrderAck:
        return "order_ack";
    case Stage::OrderFill:
        return "order_fill";
    }
    return "feed";
}

std::optional<ClockEstimate> best_of_burst(std::span<const OffsetSample> burst) noexcept {
    const OffsetSample* best = nullptr;
    std::uint64_t samples = 0;
    OffsetInterval agreed{};
    for (const auto& sample : burst) {
        if (sample.received_ns < sample.sent_ns || sample.server_ns == 0) {
            continue;
        }
        ++samples;
        agreed = intersect(agreed, sample_interval(sample));
        if (best == nullptr || sample.received_ns - sample.sent_ns < best->received_ns - best->sent_ns) {
            best = &sample;
        }
    }
    if (best == nullptr) {
        return std::nullopt;
    }
    // disjoint samples mean two clocks behind one name or a step mid-burst; trust the shortest trip alone
    if (agreed.empty()) {
        agreed = sample_interval(*best);
    }
    const auto width = static_cast<std::uint64_t>(agreed.hi - agreed.lo);
    const auto offset = agreed.lo + static_cast<std::int64_t>(width / 2);
    return ClockEstimate{.method = ClockMethod::RestProbe,
                         .offset_ns = offset,
                         .uncertainty_ns = static_cast<std::uint64_t>(agreed.hi - offset),
                         .rtt_ns = best->received_ns - best->sent_ns,
                         .resolution_ns = best->resolution_ns,
                         .sampled_ns = best->received_ns,
                         .samples = samples,
                         .drift_ns = 0,
                         .drift_over_ns = 0};
}

ClockStats clock_stats(std::string venue, std::string endpoint, const ClockEstimate& estimate) {
    return ClockStats{.venue = std::move(venue),
                      .method = std::string{to_string(estimate.method)},
                      .endpoint = std::move(endpoint),
                      .offset_ns = estimate.offset_ns,
                      .uncertainty_ns = estimate.uncertainty_ns,
                      .rtt_ns = estimate.rtt_ns,
                      .sampled_ns = estimate.sampled_ns,
                      .samples = estimate.samples,
                      .drift_ns = estimate.drift_ns,
                      .drift_over_ns = estimate.drift_over_ns};
}

LatencyStats LatencySeries::stats(std::string channel, Stage stage) const {
    auto window = current_;
    window.merge(previous_);
    const auto snap = window.snapshot();
    auto since_start = all_;
    since_start.merge(current_);
    std::vector<std::uint64_t> buckets(k_bucket_bounds_ns.size(), 0);
    std::size_t le = 0;
    std::uint64_t below = 0;
    since_start.for_each_bucket([&](std::uint64_t upper, std::uint64_t count) {
        while (le < buckets.size() && upper > k_bucket_bounds_ns[le]) {
            buckets[le++] = below;
        }
        below += count;
    });
    for (; le < buckets.size(); ++le) {
        buckets[le] = below;
    }
    return LatencyStats{.channel = std::move(channel),
                        .stage = std::string{to_string(stage)},
                        .count = snap.count,
                        .p50_ns = snap.p50_ns,
                        .p90_ns = snap.p90_ns,
                        .p99_ns = snap.p99_ns,
                        .p999_ns = snap.p999_ns,
                        .max_ns = snap.max_ns,
                        .total = total_,
                        .sum_ns = sum_,
                        .below_zero = below_zero_,
                        .buckets = std::move(buckets)};
}

LatencySeries& LatencyBook::series(std::string_view channel, Stage stage) {
    const auto it = std::ranges::find_if(
        entries_, [&](const Entry& entry) { return entry.stage == stage && entry.channel == channel; });
    if (it != entries_.end()) {
        return *it->series;
    }
    entries_.push_back(
        Entry{.channel = std::string{channel}, .stage = stage, .series = std::make_unique<LatencySeries>()});
    return *entries_.back().series;
}

void LatencyBook::rotate_if_due(std::uint64_t mono_ns) noexcept {
    if (rotate_due_ == 0) {
        rotate_due_ = mono_ns + k_slice_ns;
        return;
    }
    if (mono_ns < rotate_due_) {
        return;
    }
    rotate_due_ = mono_ns + k_slice_ns;
    for (auto& entry : entries_) {
        entry.series->rotate();
    }
}

std::vector<LatencyStats> LatencyBook::stats() const {
    std::vector<LatencyStats> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_) {
        if (entry.series != nullptr) {
            out.push_back(entry.series->stats(entry.channel, entry.stage));
        }
    }
    return out;
}

} // namespace raikiri::introspect
