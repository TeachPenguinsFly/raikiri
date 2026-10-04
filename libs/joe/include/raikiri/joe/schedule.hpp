#pragma once

#include <raikiri/joe/order.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace raikiri::joe {

[[nodiscard]] std::vector<std::uint64_t> flat_weights(std::uint32_t slices);

// heavier at both ends of the horizon: 1 + 2(2x - 1)^2 at each slice's midpoint x
[[nodiscard]] std::vector<std::uint64_t> u_weights(std::uint32_t slices);

// slice 0 fires at start; the others move by up to jitter_bp of the interval, seeded so a
// parent's schedule is the same on every call
[[nodiscard]] std::vector<UnixNanos> slice_times(
    UnixNanos start, std::uint64_t horizon_ns, std::uint32_t slices, std::uint32_t jitter_bp, std::uint64_t seed);

[[nodiscard]] std::uint32_t fired(std::span<const UnixNanos> times, UnixNanos now) noexcept;

// what should have traded once `fired` slices are out; the last slice lands on total exactly
[[nodiscard]] FixedE9 cumulative_target(FixedE9 total, std::span<const std::uint64_t> weights, std::uint32_t fired);

// the planned size of slice `index`
[[nodiscard]] FixedE9 slice_size(FixedE9 total, std::span<const std::uint64_t> weights, std::uint32_t index);

// the shortfall against `target`, capped at catchup_pct of the planned slice until the last one
[[nodiscard]] FixedE9 next_slice(
    FixedE9 target, FixedE9 filled, FixedE9 working, FixedE9 planned, std::uint32_t catchup_pct, bool last) noexcept;

[[nodiscard]] FixedE9 pov_target(FixedE9 total, FixedE9 market_volume, std::uint32_t rate_bp) noexcept;

// traded volume by half hour of the utc day, from recorded trades or live ones
class VolumeCurve {
public:
    static constexpr std::size_t k_buckets = 48;
    static constexpr std::uint64_t k_bucket_ns = 30ULL * 60 * 1'000'000'000ULL;

    void add(UnixNanos ts, FixedE9 qty) noexcept;

    [[nodiscard]] bool empty() const noexcept;

    // each slice's share of [start, start + horizon) by its overlap with each bucket; flat when empty
    [[nodiscard]] std::vector<std::uint64_t>
    weights(UnixNanos start, std::uint64_t horizon_ns, std::uint32_t slices) const;

private:
    std::array<std::uint64_t, k_buckets> volume_{};
};

} // namespace raikiri::joe
