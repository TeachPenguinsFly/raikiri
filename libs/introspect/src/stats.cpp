#include <raikiri/introspect/stats.hpp>

#include <algorithm>

namespace raikiri::introspect {

std::string_view to_string(Liveness state) noexcept {
    switch (state) {
    case Liveness::Up:
        return "up";
    case Liveness::Stale:
        return "stale";
    case Liveness::Down:
        return "down";
    }
    return "down";
}

Liveness classify(std::uint64_t taken_ns,
                  bool process_alive,
                  std::uint64_t now_ns,
                  std::uint64_t max_age_ns) noexcept {
    if (!process_alive) {
        return Liveness::Down;
    }
    return now_ns > taken_ns && now_ns - taken_ns > max_age_ns ? Liveness::Stale : Liveness::Up;
}

namespace {

void add(Rollup& into, const GatewayView& view) {
    ++into.gateways;
    if (view.state == Liveness::Down) {
        ++into.down;
        return;
    }
    const auto& stats = view.stats;
    into.instruments += stats.catalogue.instruments;
    for (const auto& sub : stats.subscriptions) {
        into.subscriptions += sub.accepted;
    }
    for (const auto& pool : stats.sockets) {
        into.sockets += pool.open;
    }
    into.messages_per_second += stats.rates.messages_per_second;
}

} // namespace

MachineRollup rollup(std::span<const GatewayView> gateways) {
    MachineRollup out{};
    out.total.venue = "all";
    for (const auto& view : gateways) {
        auto it = std::ranges::find(out.venues, view.stats.venue, &Rollup::venue);
        if (it == out.venues.end()) {
            it = out.venues.insert(out.venues.end(), Rollup{});
            it->venue = view.stats.venue;
        }
        add(*it, view);
        add(out.total, view);
    }
    std::ranges::sort(out.venues, {}, &Rollup::venue);
    return out;
}

void RateWindow::push(std::uint64_t mono_ns, const Counts& counts) noexcept {
    ring_[next_] = Sample{.mono_ns = mono_ns, .counts = counts};
    next_ = (next_ + 1) % k_samples;
    size_ = std::min(size_ + 1, k_samples);
}

const RateWindow::Sample& RateWindow::newest() const noexcept {
    return ring_[(next_ + k_samples - 1) % k_samples];
}

const RateWindow::Sample& RateWindow::oldest_in_window() const noexcept {
    const auto& head = newest();
    const Sample* pick = &head;
    for (std::size_t back = 1; back < size_; ++back) {
        const auto& sample = ring_[(next_ + k_samples - 1 - back) % k_samples];
        if (head.mono_ns - sample.mono_ns > window_ns_) {
            break;
        }
        pick = &sample;
    }
    return *pick;
}

double RateWindow::per_second(std::size_t channel) const noexcept {
    if (size_ < 2 || channel >= k_channels) {
        return 0;
    }
    const auto& head = newest();
    const auto& tail = oldest_in_window();
    if (head.mono_ns <= tail.mono_ns || head.counts[channel] < tail.counts[channel]) {
        return 0;
    }
    return static_cast<double>(head.counts[channel] - tail.counts[channel]) * 1e9 /
           static_cast<double>(head.mono_ns - tail.mono_ns);
}

double RateWindow::total_per_second() const noexcept {
    double total = 0;
    for (std::size_t channel = 0; channel < k_channels; ++channel) {
        total += per_second(channel);
    }
    return total;
}

} // namespace raikiri::introspect
