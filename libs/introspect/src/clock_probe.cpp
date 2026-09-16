#include <raikiri/introspect/clock_probe.hpp>

#include <utility>

namespace raikiri::introspect {

ProbeClock::ProbeClock(ServerTimeSource& source,
                       MonoClock now,
                       Admit admit,
                       Jitter jitter,
                       ProbeSchedule schedule)
    : source_{&source}, now_{std::move(now)}, admit_{std::move(admit)},
      jitter_{std::move(jitter)}, schedule_{schedule} {}

std::uint64_t ProbeClock::tick() {
    const auto now = now_();
    if (now < due_ns_) {
        return due_ns_;
    }
    if (!admit_()) {
        due_ns_ = now + schedule_.burst_gap_ns;
        return due_ns_;
    }
    if (const auto sample = source_->read(); !sample.has_value() || !estimator_.add(*sample)) {
        ++failures_;
    }
    ++attempts_;
    due_ns_ = attempts_ < schedule_.burst
                  ? now_() + schedule_.burst_gap_ns
                  : now_() + jitter_(schedule_.period_min_ns, schedule_.period_max_ns);
    return due_ns_;
}

} // namespace raikiri::introspect
