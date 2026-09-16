#pragma once

// DOMAIN: how far a venue's clock sits from ours, and how long its data took
// to reach us. Values and pure arithmetic; samples are handed in.

#include <raikiri/observ/histogram.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace raikiri::introspect {

// Sign: server minus local. A venue stamp converts to our clock as
// ts_event - offset, so a feed delay is ts_init - ts_event + offset.
enum class ClockMethod : std::uint8_t { None, RestProbe, FeedBound };

[[nodiscard]] std::string_view to_string(ClockMethod method) noexcept;

struct OffsetSample {
    std::uint64_t sent_ns{0};
    std::uint64_t received_ns{0};
    std::uint64_t server_ns{0};
    std::uint64_t resolution_ns{0};
};

struct ClockEstimate {
    ClockMethod method{ClockMethod::None};
    std::int64_t offset_ns{0};
    std::uint64_t uncertainty_ns{0};
    std::uint64_t rtt_ns{0};
    std::uint64_t sampled_ns{0};
    std::uint64_t samples{0};
};

// NTP's clock filter: of the last k_window probes, trust the one with the
// shortest round trip, since queueing only ever adds delay.
class OffsetEstimator {
public:
    static constexpr std::size_t k_window = 8;

    bool add(const OffsetSample& sample) noexcept;
    [[nodiscard]] ClockEstimate estimate() const noexcept;

private:
    struct Kept {
        std::int64_t offset_ns{0};
        std::uint64_t rtt_ns{0};
        std::uint64_t resolution_ns{0};
        std::uint64_t sampled_ns{0};
    };

    std::array<Kept, k_window> ring_{};
    std::size_t next_{0};
    std::size_t size_{0};
    std::uint64_t samples_{0};
};

// For a venue with no time endpoint. Each message gives ts_init - ts_event =
// one-way latency - offset, and latency is never negative, so the smallest
// difference seen bounds the offset from below.
class FeedBound {
public:
    static constexpr std::int64_t k_plausible_ns = 3'600'000'000'000LL;

    void add(std::int64_t init_minus_event_ns) noexcept {
        if (init_minus_event_ns > -k_plausible_ns && init_minus_event_ns < k_plausible_ns &&
            init_minus_event_ns < current_) {
            current_ = init_minus_event_ns;
        }
    }
    void rotate() noexcept {
        previous_ = current_;
        current_ = k_none;
    }
    [[nodiscard]] ClockEstimate estimate(std::uint64_t now_ns) const noexcept;

private:
    static constexpr std::int64_t k_none = k_plausible_ns;
    std::int64_t current_{k_none};
    std::int64_t previous_{k_none};
};

// Upper bounds of the exported cumulative buckets: five a decade, 1 us to 10 s.
inline constexpr std::array<std::uint64_t, 36> k_bucket_bounds_ns = [] {
    constexpr std::array<std::uint64_t, 5> mantissa{1000, 1585, 2512, 3981, 6310};
    std::array<std::uint64_t, 36> out{};
    std::uint64_t scale = 1;
    for (std::size_t i = 0; i < 35; ++i) {
        out[i] = mantissa[i % 5] * scale;
        if (i % 5 == 4) {
            scale *= 10;
        }
    }
    out[35] = 10'000'000'000ULL;
    return out;
}();

enum class Stage : std::uint8_t {
    Feed,
    Internal,
    KernelQueue,
    Rest,
    WsPing,
    OrderAck,
    OrderFill
};

[[nodiscard]] std::string_view to_string(Stage stage) noexcept;

struct LatencyStats {
    std::string channel;
    std::string stage;
    std::uint64_t count{0};
    std::uint64_t p50_ns{0};
    std::uint64_t p90_ns{0};
    std::uint64_t p99_ns{0};
    std::uint64_t p999_ns{0};
    std::uint64_t max_ns{0};
    std::uint64_t total{0};
    std::uint64_t sum_ns{0};
    std::uint64_t below_zero{0};
    // Since start, how many were at or below each of k_bucket_bounds_ns.
    std::vector<std::uint64_t> buckets;
};

struct ClockStats {
    std::string venue;
    std::string method{"none"};
    std::string endpoint;
    std::int64_t offset_ns{0};
    std::uint64_t uncertainty_ns{0};
    std::uint64_t rtt_ns{0};
    std::uint64_t sampled_ns{0};
    std::uint64_t samples{0};
};

[[nodiscard]] ClockStats clock_stats(std::string venue,
                                     std::string endpoint,
                                     const ClockEstimate& estimate);

// Two slices of a trailing window: quantiles cover the last one to two
// rotations; count, sum and buckets since start. Fixed memory, so recording never
// allocates. A negative value is clamped to zero and counted.
class LatencySeries {
public:
    void record(std::int64_t ns) noexcept {
        if (ns < 0) {
            ++below_zero_;
            ns = 0;
        }
        const auto value = static_cast<std::uint64_t>(ns);
        current_.record(value);
        ++total_;
        sum_ += value;
    }
    void rotate() noexcept {
        all_.merge(current_);
        previous_ = current_;
        current_ = observ::Histogram{};
    }
    [[nodiscard]] LatencyStats stats(std::string channel, Stage stage) const;

private:
    observ::Histogram current_;
    observ::Histogram previous_;
    observ::Histogram all_;
    std::uint64_t total_{0};
    std::uint64_t sum_{0};
    std::uint64_t below_zero_{0};
};

// Every series one gateway keeps, found by name once and then held by
// pointer. Single-threaded; a caller sharing one across threads locks it.
class LatencyBook {
public:
    static constexpr std::uint64_t k_slice_ns = 30ULL * 1'000'000'000ULL;

    [[nodiscard]] LatencySeries& series(std::string_view channel, Stage stage);
    void rotate_if_due(std::uint64_t mono_ns) noexcept;
    [[nodiscard]] std::vector<LatencyStats> stats() const;

private:
    struct Entry {
        std::string channel;
        Stage stage{Stage::Feed};
        std::unique_ptr<LatencySeries> series;
    };

    std::vector<Entry> entries_;
    std::uint64_t rotate_due_{0};
};

} // namespace raikiri::introspect
