#include <raikiri/introspect/clock_probe.hpp>

#include <utility>
#include <vector>

namespace raikiri::introspect {

ClockSync::ClockSync(ServerTimeSource& source, MonoClock now, Admit admit, Pause pause, ProbeSchedule schedule)
    : source_{&source}, now_{std::move(now)}, admit_{std::move(admit)}, pause_{std::move(pause)}, schedule_{schedule} {}

std::uint64_t ClockSync::tick() {
    if (now_() < due_ns_) {
        return due_ns_;
    }
    std::vector<OffsetSample> burst;
    burst.reserve(schedule_.burst);
    for (std::uint32_t i = 0; i < schedule_.burst; ++i) {
        if (i != 0 && !pause_(schedule_.burst_gap_ns)) {
            break;
        }
        if (!admit_()) {
            continue;
        }
        if (auto sample = source_->read(); sample.has_value()) {
            burst.push_back(*sample);
        }
    }
    auto best = best_of_burst(burst);
    if (!best.has_value()) {
        ++failures_;
        due_ns_ = now_() + schedule_.retry_ns;
        return due_ns_;
    }
    if (estimate_.method != ClockMethod::None) {
        best->drift_ns = best->offset_ns - estimate_.offset_ns;
        best->drift_over_ns = best->sampled_ns - estimate_.sampled_ns;
    }
    estimate_ = *best;
    due_ns_ = now_() + schedule_.period_ns;
    return due_ns_;
}

} // namespace raikiri::introspect
