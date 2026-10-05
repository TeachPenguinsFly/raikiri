#pragma once

// when to measure a venue's clock and how to read a burst; the clock and the pauses are injected

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
    std::uint32_t burst{8};
    std::uint64_t burst_gap_ns{200'000'000ULL};
    std::uint64_t period_ns{3'600'000'000'000ULL};
    std::uint64_t retry_ns{60'000'000'000ULL};
};

class ClockSync {
public:
    using MonoClock = std::function<std::uint64_t()>;
    // false skips that request
    using Admit = std::function<bool()>;
    // false when the caller is stopping
    using Pause = std::function<bool(std::uint64_t ns)>;

    ClockSync(ServerTimeSource& source, MonoClock now, Admit admit, Pause pause, ProbeSchedule schedule = {});

    // measures when due; returns the monotonic time of the next burst
    std::uint64_t tick();

    // method None until a burst has answered
    [[nodiscard]] const ClockEstimate& estimate() const noexcept { return estimate_; }

    [[nodiscard]] std::uint64_t failures() const noexcept { return failures_; }

private:
    ServerTimeSource* source_;
    MonoClock now_;
    Admit admit_;
    Pause pause_;
    ProbeSchedule schedule_;
    ClockEstimate estimate_{};
    std::uint64_t due_ns_{0};
    std::uint64_t failures_{0};
};

} // namespace raikiri::introspect
