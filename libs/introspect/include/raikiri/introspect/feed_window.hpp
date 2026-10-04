#pragma once

// per-channel message counts and feed lag over a rolling minute; fixed memory, single writer

#include <array>
#include <cstddef>
#include <cstdint>

namespace raikiri::introspect {

// signed and log-bucketed: under 1 us is zero, then four buckets per power of two up to about 67 s
class LagHistogram {
public:
    static constexpr std::size_t k_octaves = 26;
    static constexpr std::size_t k_per_octave = 4;
    static constexpr std::size_t k_side = k_octaves * k_per_octave;
    static constexpr std::size_t k_buckets = 2 * k_side + 1;

    void record(std::int64_t ns) noexcept {
        ++counts_[index_of(ns)];
        ++count_;
    }

    void merge(const LagHistogram& other) noexcept {
        for (std::size_t i = 0; i < k_buckets; ++i) {
            counts_[i] += other.counts_[i];
        }
        count_ += other.count_;
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }

    // the middle of the bucket holding that rank; zero when empty
    [[nodiscard]] std::int64_t quantile(std::uint32_t permille) const noexcept;

    [[nodiscard]] static std::size_t index_of(std::int64_t ns) noexcept;
    [[nodiscard]] static std::int64_t middle_of(std::size_t index) noexcept;

private:
    std::array<std::uint32_t, k_buckets> counts_{};
    std::uint64_t count_{0};
};

struct FeedSummary {
    std::uint64_t messages{0};
    std::uint64_t window_ns{0};
    std::uint64_t lag_samples{0};
    std::int64_t lag_p50_ns{0};
    std::int64_t lag_p99_ns{0};
    std::uint64_t last_recv_ns{0};
};

class FeedWindow {
public:
    static constexpr std::size_t k_slices = 6;
    static constexpr std::uint64_t k_slice_ns = 10'000'000'000ULL;

    // a stamped message: ts_recv - ts_event on the local clock
    void record(std::int64_t lag_ns, std::uint64_t recv_ns) noexcept {
        auto& slice = slices_[head_];
        ++slice.messages;
        slice.lag.record(lag_ns);
        last_recv_ns_ = recv_ns;
    }

    // a message the venue did not stamp
    void count(std::uint64_t recv_ns) noexcept {
        ++slices_[head_].messages;
        last_recv_ns_ = recv_ns;
    }

    void advance(std::uint64_t mono_ns) noexcept;

    [[nodiscard]] FeedSummary summary(std::uint64_t mono_ns) const noexcept;

private:
    struct Slice {
        std::uint64_t messages{0};
        LagHistogram lag;
    };

    std::array<Slice, k_slices> slices_{};
    std::size_t head_{0};
    std::size_t full_{0};
    std::uint64_t head_start_ns_{0};
    std::uint64_t last_recv_ns_{0};
};

} // namespace raikiri::introspect
