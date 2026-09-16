#pragma once

// APPLICATION: when to ask a venue for its time, and what to make of the
// answers. The asking is a port; the pacing runs off an injected clock.

#include <raikiri/introspect/latency.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace raikiri::introspect {

class ServerTimeSource {
public:
    ServerTimeSource() = default;
    ServerTimeSource(const ServerTimeSource&) = delete;
    ServerTimeSource& operator=(const ServerTimeSource&) = delete;
    virtual ~ServerTimeSource() = default;

    [[nodiscard]] virtual std::string endpoint() const = 0;
    [[nodiscard]] virtual std::optional<OffsetSample> read() = 0;
};

struct ProbeSchedule {
    std::uint32_t burst{4};
    std::uint64_t burst_gap_ns{2'000'000'000ULL};
    std::uint64_t period_min_ns{30'000'000'000ULL};
    std::uint64_t period_max_ns{60'000'000'000ULL};
};

class ProbeClock {
public:
    using MonoClock = std::function<std::uint64_t()>;
    // A false refuses this round; it is asked again on the next.
    using Admit = std::function<bool()>;
    // Uniform in [lo, hi].
    using Jitter = std::function<std::uint64_t(std::uint64_t lo, std::uint64_t hi)>;

    ProbeClock(ServerTimeSource& source,
               MonoClock now,
               Admit admit,
               Jitter jitter,
               ProbeSchedule schedule = {});

    // Probes when due. Returns the monotonic instant of the next attempt.
    std::uint64_t tick();

    [[nodiscard]] ClockEstimate estimate() const noexcept { return estimator_.estimate(); }
    [[nodiscard]] std::uint64_t failures() const noexcept { return failures_; }

private:
    ServerTimeSource* source_;
    MonoClock now_;
    Admit admit_;
    Jitter jitter_;
    ProbeSchedule schedule_;
    OffsetEstimator estimator_;
    std::uint64_t due_ns_{0};
    std::uint32_t attempts_{0};
    std::uint64_t failures_{0};
};

} // namespace raikiri::introspect
