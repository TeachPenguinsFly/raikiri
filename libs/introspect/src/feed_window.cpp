#include <raikiri/introspect/feed_window.hpp>

#include <algorithm>
#include <bit>

namespace raikiri::introspect {

std::size_t LagHistogram::index_of(std::int64_t ns) noexcept {
    if (ns > -1000 && ns < 1000) {
        return k_side;
    }
    const auto magnitude = ns < 0 ? 0 - static_cast<std::uint64_t>(ns) : static_cast<std::uint64_t>(ns);
    const auto us = magnitude / 1000;
    auto octave = static_cast<std::size_t>(std::bit_width(us)) - 1;
    std::size_t sub = 0;
    if (octave >= k_octaves) {
        octave = k_octaves - 1;
        sub = k_per_octave - 1;
    } else {
        const auto base = 1000ULL << octave;
        sub = static_cast<std::size_t>((magnitude - base) * k_per_octave / base);
    }
    const auto side = octave * k_per_octave + sub + 1;
    return ns > 0 ? k_side + side : k_side - side;
}

std::int64_t LagHistogram::middle_of(std::size_t index) noexcept {
    if (index == k_side) {
        return 0;
    }
    const auto side = index > k_side ? index - k_side : k_side - index;
    const auto octave = (side - 1) / k_per_octave;
    const auto sub = (side - 1) % k_per_octave;
    const auto base = 1000ULL << octave;
    const auto width = base / k_per_octave;
    const auto middle = static_cast<std::int64_t>(base + sub * width + width / 2);
    return index > k_side ? middle : -middle;
}

std::int64_t LagHistogram::quantile(std::uint32_t permille) const noexcept {
    if (count_ == 0) {
        return 0;
    }
    const auto rank = std::max<std::uint64_t>(1, (count_ * permille + 999) / 1000);
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < k_buckets; ++i) {
        if (seen + counts_[i] >= rank) {
            if (i == k_side) {
                return 0;
            }
            // linear inside the bucket, so a steady tail does not read as one repeated bucket middle
            const auto side = i > k_side ? i - k_side : k_side - i;
            const auto base = 1000ULL << ((side - 1) / k_per_octave);
            const auto width = base / k_per_octave;
            const auto near = base + ((side - 1) % k_per_octave) * width;
            const auto into = (2 * (rank - seen) - 1) * 1000 / (2 * counts_[i]);
            const auto offset = i > k_side ? into : 1000 - into;
            const auto magnitude = static_cast<std::int64_t>(near + width * offset / 1000);
            return i > k_side ? magnitude : -magnitude;
        }
        seen += counts_[i];
    }
    return middle_of(k_buckets - 1);
}

void FeedWindow::advance(std::uint64_t mono_ns) noexcept {
    if (head_start_ns_ == 0) {
        head_start_ns_ = mono_ns;
        return;
    }
    if (mono_ns < head_start_ns_ + k_slice_ns) {
        return;
    }
    const auto steps = (mono_ns - head_start_ns_) / k_slice_ns;
    if (steps >= k_slices) {
        slices_ = {};
        head_ = 0;
        full_ = 0;
        head_start_ns_ = mono_ns;
        return;
    }
    for (std::uint64_t i = 0; i < steps; ++i) {
        head_ = (head_ + 1) % k_slices;
        slices_[head_] = Slice{};
        full_ = std::min(full_ + 1, k_slices - 1);
    }
    head_start_ns_ += steps * k_slice_ns;
}

FeedSummary FeedWindow::summary(std::uint64_t mono_ns) const noexcept {
    LagHistogram lag;
    std::uint64_t messages = 0;
    for (const auto& slice : slices_) {
        messages += slice.messages;
        lag.merge(slice.lag);
    }
    const auto in_head = head_start_ns_ != 0 && mono_ns > head_start_ns_ ? mono_ns - head_start_ns_ : 0;
    return FeedSummary{.messages = messages,
                       .window_ns = full_ * k_slice_ns + in_head,
                       .lag_samples = lag.count(),
                       .lag_p50_ns = lag.quantile(500),
                       .lag_p99_ns = lag.quantile(990),
                       .last_recv_ns = last_recv_ns_};
}

} // namespace raikiri::introspect
