#include <raikiri/bookrisk/track.hpp>

#include <algorithm>

namespace raikiri::bookrisk {

void BookTrack::on_fill(InstrumentId instrument) noexcept {
    if (filled_since_entry(instrument)) {
        return;
    }
    filled_[next_filled_] = instrument.packed;
    next_filled_ = (next_filled_ + 1) % filled_.size();
}

bool BookTrack::filled_since_entry(InstrumentId instrument) const noexcept {
    return std::ranges::find(filled_, instrument.packed) != filled_.end();
}

void BookTrack::fill(const OrderCmd& cmd, const oms::RiskConfig& config, oms::Exposure& exposure) const noexcept {
    const auto now = cmd.ts_created.ns;
    exposure.orders_in_window = orders_.count(now, config.order_window_ns);
    exposure.repeats_in_window = repeats_.count(now, config.repeat_window_ns);
    exposure.filled_since_entry = filled_since_entry(cmd.instrument);
}

void BookTrack::on_sent(const OrderCmd& cmd, const oms::Exposure& exposure) noexcept {
    orders_.push(cmd.ts_created.ns);
    if (cmd.kind != CmdKind::Create || !oms::grows_worst_case(cmd, exposure)) {
        return;
    }
    if (const auto it = std::ranges::find(filled_, cmd.instrument.packed); it != filled_.end()) {
        *it = 0;
        repeats_.push(cmd.ts_created.ns);
    }
}

void BookTrack::forgive() noexcept {
    repeats_.clear();
    filled_.fill(0);
}

void LossSpeed::sample(UnixNanos at, FixedE9 pnl) noexcept {
    samples_[next_] = {.at_ns = at.ns, .pnl = pnl};
    next_ = (next_ + 1) % samples_.size();
    used_ = used_ < samples_.size() ? used_ + 1 : used_;
}

LossChange LossSpeed::change(UnixNanos now, std::uint64_t window_ns) const noexcept {
    if (used_ == 0) {
        return {};
    }
    const auto from = now.ns > window_ns ? now.ns - window_ns : 0;
    // the latest sample at or before the window's start, else the oldest there is
    std::size_t low = 0;
    std::size_t high = used_;
    while (low < high) {
        const auto mid = low + (high - low) / 2;
        if (nth_oldest(mid).at_ns <= from) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    const auto& then = nth_oldest(low == 0 ? 0 : low - 1);
    const auto& latest = nth_oldest(used_ - 1);
    return {.change = latest.pnl - then.pnl, .covered_ns = latest.at_ns > then.at_ns ? latest.at_ns - then.at_ns : 0};
}

} // namespace raikiri::bookrisk
