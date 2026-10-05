#pragma once

// venue clock offset and data latency; pure values

#include <raikiri/observ/histogram.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace raikiri::introspect {

// sign: server minus local; feed delay is ts_init - ts_event + offset
enum class ClockMethod : std::uint8_t { None, RestProbe };

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
    std::uint64_t resolution_ns{0};
    std::uint64_t sampled_ns{0};
    std::uint64_t samples{0};
    // offset change since the previous measurement, and the time between the two
    std::int64_t drift_ns{0};
    std::uint64_t drift_over_ns{0};
};

// the intersection of every sample's bounds, centred; rtt and time are the shortest trip's
// backwards or unstamped samples do not count
[[nodiscard]] std::optional<ClockEstimate> best_of_burst(std::span<const OffsetSample> burst) noexcept;

// cumulative bucket upper bounds: five a decade, 1 us to 10 s
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

enum class Stage : std::uint8_t { Feed, Internal, KernelQueue, Rest, WsPing, OrderAck, OrderFill };

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
    // since start, counts at or below each of k_bucket_bounds_ns
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
    std::int64_t drift_ns{0};
    std::uint64_t drift_over_ns{0};
};

[[nodiscard]] ClockStats clock_stats(std::string venue, std::string endpoint, const ClockEstimate& estimate);

// quantiles cover the last one to two rotations; count, sum and buckets since start
// fixed memory; negatives are clamped to zero and counted
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

// every series one gateway keeps; single-threaded
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
