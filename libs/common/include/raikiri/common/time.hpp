#pragma once

// UnixNanos is CLOCK_REALTIME, MonoNanos is CLOCK_MONOTONIC; components take a Clock& so tests can
// drive time

#include <atomic>
#include <compare>
#include <cstdint>
#include <ctime>

namespace raikiri {

struct UnixNanos {
    std::uint64_t ns{0};

    friend constexpr auto operator<=>(UnixNanos, UnixNanos) noexcept = default;

    [[nodiscard]] friend constexpr UnixNanos operator+(UnixNanos t, std::uint64_t d) noexcept {
        return {t.ns + d};
    }
};

struct MonoNanos {
    std::uint64_t ns{0};

    friend constexpr auto operator<=>(MonoNanos, MonoNanos) noexcept = default;

    [[nodiscard]] friend constexpr MonoNanos operator+(MonoNanos t, std::uint64_t d) noexcept {
        return {t.ns + d};
    }
};

class Clock {
public:
    Clock() = default;
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    virtual ~Clock() = default;

    [[nodiscard]] virtual UnixNanos wall() const noexcept = 0;
    [[nodiscard]] virtual MonoNanos mono() const noexcept = 0;
};

class LiveClock final : public Clock {
public:
    [[nodiscard]] UnixNanos wall() const noexcept override {
        timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        return {static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000U +
                static_cast<std::uint64_t>(ts.tv_nsec)};
    }

    [[nodiscard]] MonoNanos mono() const noexcept override {
#if defined(__APPLE__)
        return {clock_gettime_nsec_np(CLOCK_UPTIME_RAW)};
#else
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return {static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000U +
                static_cast<std::uint64_t>(ts.tv_nsec)};
#endif
    }
};

class TestClock final : public Clock {
public:
    explicit TestClock(UnixNanos start_wall = {}, MonoNanos start_mono = {}) noexcept
        : wall_ns_{start_wall.ns}, mono_ns_{start_mono.ns} {}

    [[nodiscard]] UnixNanos wall() const noexcept override {
        return {wall_ns_.load(std::memory_order_relaxed)};
    }

    [[nodiscard]] MonoNanos mono() const noexcept override {
        return {mono_ns_.load(std::memory_order_relaxed)};
    }

    void advance(std::uint64_t delta_ns) noexcept {
        wall_ns_.fetch_add(delta_ns, std::memory_order_relaxed);
        mono_ns_.fetch_add(delta_ns, std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> wall_ns_;
    std::atomic<std::uint64_t> mono_ns_;
};

} // namespace raikiri
