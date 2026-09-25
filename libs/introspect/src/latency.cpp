#include <raikiri/introspect/latency.hpp>

#include <algorithm>

namespace raikiri::introspect {

std::string_view to_string(ClockMethod method) noexcept {
    switch (method) {
    case ClockMethod::None:
        return "none";
    case ClockMethod::RestProbe:
        return "rest_probe";
    case ClockMethod::FeedBound:
        return "feed_bound";
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

bool OffsetEstimator::add(const OffsetSample& sample) noexcept {
    if (sample.received_ns < sample.sent_ns || sample.server_ns == 0) {
        return false;
    }
    const auto rtt = sample.received_ns - sample.sent_ns;
    const auto midpoint = sample.sent_ns + rtt / 2;
    // a truncated server stamp sits half a tick before the instant it names
    const auto server = sample.server_ns + sample.resolution_ns / 2;
    ring_[next_] = Kept{.offset_ns = static_cast<std::int64_t>(server) - static_cast<std::int64_t>(midpoint),
                        .rtt_ns = rtt,
                        .resolution_ns = sample.resolution_ns,
                        .sampled_ns = sample.received_ns};
    next_ = (next_ + 1) % k_window;
    size_ = std::min(size_ + 1, k_window);
    ++samples_;
    return true;
}

ClockEstimate OffsetEstimator::estimate() const noexcept {
    if (size_ == 0) {
        return {};
    }
    const Kept* best = &ring_[0];
    for (std::size_t i = 1; i < size_; ++i) {
        if (ring_[i].rtt_ns < best->rtt_ns) {
            best = &ring_[i];
        }
    }
    return ClockEstimate{.method = ClockMethod::RestProbe,
                         .offset_ns = best->offset_ns,
                         .uncertainty_ns = best->rtt_ns / 2 + best->resolution_ns / 2,
                         .rtt_ns = best->rtt_ns,
                         .sampled_ns = best->sampled_ns,
                         .samples = samples_};
}

ClockEstimate FeedBound::estimate(std::uint64_t now_ns) const noexcept {
    const auto least = std::min(current_, previous_);
    if (least == k_none) {
        return {};
    }
    return ClockEstimate{.method = ClockMethod::FeedBound,
                         .offset_ns = -least,
                         .uncertainty_ns = 0,
                         .rtt_ns = 0,
                         .sampled_ns = now_ns,
                         .samples = 0};
}

ClockStats clock_stats(std::string venue, std::string endpoint, const ClockEstimate& estimate) {
    return ClockStats{.venue = std::move(venue),
                      .method = std::string{to_string(estimate.method)},
                      .endpoint = std::move(endpoint),
                      .offset_ns = estimate.offset_ns,
                      .uncertainty_ns = estimate.uncertainty_ns,
                      .rtt_ns = estimate.rtt_ns,
                      .sampled_ns = estimate.sampled_ns,
                      .samples = estimate.samples};
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
