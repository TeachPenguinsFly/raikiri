#pragma once

// Log-bucketed latency histogram, HdrHistogram-shaped but dependency-free.
//
// Layout: 64 major buckets (one per power of two of nanoseconds) × 32 minor
// linear sub-buckets = 2048 u64 counters, 16 KiB per instance. Relative
// error is bounded by 1/32 (~3%) of the value — plenty for p50/p99/p999
// reporting where the interesting differences are multiples, not percents.
//
// Concurrency contract: SINGLE WRITER. One instance per hot thread; a cold
// thread gets data via snapshot() called from the owning thread's cadence
// work (or after the thread quiesces). No atomics on record() — that is the
// entire point: recording is a handful of arithmetic ops and one increment.

#include <bit>
#include <cstddef>
#include <cstdint>

namespace raikiri::observ {

struct HistogramSnapshot {
    std::uint64_t count{0};
    std::uint64_t max_ns{0};
    std::uint64_t p50_ns{0};
    std::uint64_t p90_ns{0};
    std::uint64_t p99_ns{0};
    std::uint64_t p999_ns{0};
};

class Histogram {
public:
    static constexpr std::size_t k_majors = 64;
    static constexpr std::size_t k_minors = 32; // per major; power of two
    static constexpr std::uint64_t k_minor_shift = 5;

    void record(std::uint64_t value_ns) noexcept {
        ++counts_[index_of(value_ns)];
        ++count_;
        if (value_ns > max_) {
            max_ = value_ns;
        }
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }

    // Percentiles report the upper bound of the containing bucket: latency
    // numbers err pessimistic, never flattering.
    [[nodiscard]] HistogramSnapshot snapshot() const noexcept {
        HistogramSnapshot snap{};
        snap.count = count_;
        snap.max_ns = max_;
        if (count_ == 0) {
            return snap;
        }
        snap.p50_ns = value_at(count_ / 2 + 1);
        snap.p90_ns = value_at(ranked(90, 100));
        snap.p99_ns = value_at(ranked(99, 100));
        snap.p999_ns = value_at(ranked(999, 1000));
        return snap;
    }

    [[nodiscard]] HistogramSnapshot snapshot_and_reset() noexcept {
        const auto snap = snapshot();
        *this = Histogram{};
        return snap;
    }

private:
    [[nodiscard]] static std::size_t index_of(std::uint64_t v) noexcept {
        if (v < k_minors) {
            return static_cast<std::size_t>(v); // exact below 32 ns
        }
        const auto major = static_cast<std::uint64_t>(63 - std::countl_zero(v));
        const auto minor = (v >> (major - k_minor_shift)) & (k_minors - 1);
        return static_cast<std::size_t>(major * k_minors + minor);
    }

    // Upper bound of bucket i (inclusive): first value of bucket i+1, minus 1.
    [[nodiscard]] static std::uint64_t upper_bound_of(std::size_t i) noexcept {
        const auto major = static_cast<std::uint64_t>(i) / k_minors;
        const auto minor = static_cast<std::uint64_t>(i) % k_minors;
        if (major < k_minor_shift + 1) {
            return i < k_minors ? i : ((1ULL << major) + 0); // small exact zone
        }
        const auto base = 1ULL << major;
        const auto step = 1ULL << (major - k_minor_shift);
        return base + (minor + 1) * step - 1;
    }

    [[nodiscard]] std::uint64_t ranked(std::uint64_t num, std::uint64_t den) const noexcept {
        const auto rank = (count_ * num + den - 1) / den; // ceil
        return rank == 0 ? 1 : rank;
    }

    [[nodiscard]] std::uint64_t value_at(std::uint64_t rank) const noexcept {
        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < k_majors * k_minors; ++i) {
            cumulative += counts_[i];
            if (cumulative >= rank) {
                const auto upper = upper_bound_of(i);
                return upper < max_ ? upper : max_;
            }
        }
        return max_;
    }

    std::uint64_t counts_[k_majors * k_minors]{};
    std::uint64_t count_{0};
    std::uint64_t max_{0};
};

} // namespace raikiri::observ
