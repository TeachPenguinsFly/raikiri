#include <raikiri/joe/schedule.hpp>

#include <algorithm>
#include <numeric>

namespace raikiri::joe {
namespace internal {

[[nodiscard]] std::uint64_t splitmix(std::uint64_t& state) noexcept {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

[[nodiscard]] std::uint64_t sum(std::span<const std::uint64_t> weights, std::size_t count) noexcept {
    return std::accumulate(weights.begin(), weights.begin() + static_cast<std::ptrdiff_t>(count), std::uint64_t{0});
}

} // namespace internal

std::vector<std::uint64_t> flat_weights(std::uint32_t slices) {
    return std::vector<std::uint64_t>(slices, 1);
}

std::vector<std::uint64_t> u_weights(std::uint32_t slices) {
    std::vector<std::uint64_t> out(slices);
    const auto n = static_cast<std::int64_t>(slices);
    for (std::int64_t i = 0; i < n; ++i) {
        const auto d = 2 * i + 1 - n;
        out[static_cast<std::size_t>(i)] = static_cast<std::uint64_t>(n * n + 2 * d * d);
    }
    return out;
}

std::vector<UnixNanos> slice_times(
    UnixNanos start, std::uint64_t horizon_ns, std::uint32_t slices, std::uint32_t jitter_bp, std::uint64_t seed) {
    std::vector<UnixNanos> out;
    if (slices == 0) {
        return out;
    }
    out.reserve(slices);
    const auto interval = horizon_ns / slices;
    // half an interval at most, so the slices keep their order
    const auto reach = interval * std::min<std::uint64_t>(jitter_bp, 5000) / 10'000;
    auto state = seed;
    for (std::uint32_t i = 0; i < slices; ++i) {
        auto at = start.ns + interval * i;
        if (i > 0 && reach > 0) {
            const auto shift = internal::splitmix(state) % (2 * reach + 1);
            at = at + shift - reach;
        }
        out.push_back(UnixNanos{at});
    }
    return out;
}

std::uint32_t fired(std::span<const UnixNanos> times, UnixNanos now) noexcept {
    return static_cast<std::uint32_t>(std::ranges::count_if(times, [now](UnixNanos t) { return t <= now; }));
}

FixedE9 cumulative_target(FixedE9 total, std::span<const std::uint64_t> weights, std::uint32_t fired) {
    if (fired >= weights.size()) {
        return total;
    }
    const auto all = internal::sum(weights, weights.size());
    if (all == 0) {
        return FixedE9::from_raw(0);
    }
    const auto done = internal::sum(weights, fired);
    return FixedE9::from_raw(static_cast<std::int64_t>(static_cast<int128_t>(total.raw) * static_cast<int128_t>(done) /
                                                       static_cast<int128_t>(all)));
}

FixedE9 slice_size(FixedE9 total, std::span<const std::uint64_t> weights, std::uint32_t index) {
    return cumulative_target(total, weights, index + 1) - cumulative_target(total, weights, index);
}

FixedE9 next_slice(
    FixedE9 target, FixedE9 filled, FixedE9 working, FixedE9 planned, std::uint32_t catchup_pct, bool last) noexcept {
    const auto shortfall = target.raw - filled.raw - working.raw;
    if (shortfall <= 0) {
        return FixedE9::from_raw(0);
    }
    if (last) {
        return FixedE9::from_raw(shortfall);
    }
    const auto cap = static_cast<std::int64_t>(static_cast<int128_t>(planned.raw) * catchup_pct / 100);
    return FixedE9::from_raw(std::min(shortfall, cap));
}

FixedE9 pov_target(FixedE9 total, FixedE9 market_volume, std::uint32_t rate_bp) noexcept {
    const auto due = static_cast<int128_t>(market_volume.raw) * rate_bp / 10'000;
    return due >= total.raw ? total : FixedE9::from_raw(static_cast<std::int64_t>(due));
}

void VolumeCurve::add(UnixNanos ts, FixedE9 qty) noexcept {
    if (qty.is_undef() || qty.raw <= 0) {
        return;
    }
    volume_[(ts.ns / k_bucket_ns) % k_buckets] += static_cast<std::uint64_t>(qty.raw);
}

bool VolumeCurve::empty() const noexcept {
    return std::ranges::all_of(volume_, [](std::uint64_t v) { return v == 0; });
}

std::vector<std::uint64_t> VolumeCurve::weights(UnixNanos start, std::uint64_t horizon_ns, std::uint32_t slices) const {
    if (slices == 0) {
        return {};
    }
    std::vector<std::uint64_t> out(slices, 0);
    const auto interval = horizon_ns / slices;
    for (std::uint32_t i = 0; i < slices; ++i) {
        auto at = start.ns + interval * i;
        const auto end = at + interval;
        uint128_t weight = 0;
        while (at < end) {
            const auto bucket_end = (at / k_bucket_ns + 1) * k_bucket_ns;
            const auto upto = std::min(bucket_end, end);
            weight += static_cast<uint128_t>(volume_[(at / k_bucket_ns) % k_buckets]) * (upto - at) / k_bucket_ns;
            at = upto;
        }
        out[i] = static_cast<std::uint64_t>(std::min<uint128_t>(weight, ~std::uint64_t{0} / slices));
    }
    if (std::ranges::all_of(out, [](std::uint64_t w) { return w == 0; })) {
        return flat_weights(slices);
    }
    return out;
}

} // namespace raikiri::joe
