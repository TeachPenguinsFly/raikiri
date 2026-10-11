#include <raikiri/options/pricing.hpp>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <vector>

namespace raikiri::options {

namespace internal {

constexpr std::int64_t k_ns_per_year = 365LL * 86'400LL * 1'000'000'000LL;

struct Implied {
    std::int64_t distance{0};
    std::int64_t forward{0};
};

// nullopt past i64, since the quotes come from a venue
std::optional<std::int64_t> scaled(std::int64_t a, std::int64_t b, std::int64_t d) noexcept {
    if (d <= 0) {
        return std::nullopt;
    }
    const int128_t out = static_cast<int128_t>(a) * b / d;
    if (out > std::numeric_limits<std::int64_t>::max() || out < std::numeric_limits<std::int64_t>::min()) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(out);
}

} // namespace internal

FixedE9 years_between(UnixNanos now, UnixNanos expiry) noexcept {
    if (expiry.ns <= now.ns) {
        return FixedE9::undef();
    }
    const auto left = static_cast<std::int64_t>(expiry.ns - now.ns);
    return FixedE9::from_raw(mul_div(left, k_e9, internal::k_ns_per_year));
}

std::optional<FixedE9> forward_from_parity(std::span<const ParityQuote> quotes,
                                           OptionUnit unit,
                                           FixedE9 discount,
                                           std::size_t strikes) noexcept {
    if (discount.is_undef() || discount.raw <= 0 || strikes == 0) {
        return std::nullopt;
    }
    std::vector<internal::Implied> implied;
    implied.reserve(quotes.size());
    for (const auto& q : quotes) {
        if (q.strike.is_undef() || q.call.is_undef() || q.put.is_undef() || q.strike.raw <= 0 || q.call.raw < 0 ||
            q.put.raw < 0) {
            continue;
        }
        const std::int64_t diff = q.call.raw - q.put.raw;
        const auto forward = unit == OptionUnit::Base
                                 ? internal::scaled(q.strike.raw, k_e9, k_e9 - diff)
                                 : internal::scaled(diff, k_e9, discount.raw).transform([&q](std::int64_t carry) {
                                       return q.strike.raw + carry;
                                   });
        if (forward.has_value() && *forward > 0) {
            implied.push_back({.distance = std::abs(diff), .forward = *forward});
        }
    }
    if (implied.empty()) {
        return std::nullopt;
    }
    const auto take = std::min(strikes, implied.size());
    std::ranges::partial_sort(
        implied, implied.begin() + static_cast<std::ptrdiff_t>(take), {}, &internal::Implied::distance);
    std::vector<std::int64_t> near;
    near.reserve(take);
    for (std::size_t i = 0; i < take; ++i) {
        near.push_back(implied[i].forward);
    }
    std::ranges::sort(near);
    const auto mid = take / 2;
    const auto median = take % 2 == 1 ? near[mid] : near[mid - 1] + (near[mid] - near[mid - 1]) / 2;
    return FixedE9::from_raw(median);
}

Forward pick_forward(const ForwardSources& sources) noexcept {
    const auto usable = [](FixedE9 v) { return !v.is_undef() && v.raw > 0; };
    if (sources.parity.has_value() && usable(*sources.parity)) {
        return {.value = *sources.parity, .source = ForwardSource::Parity};
    }
    if (sources.future.has_value() && usable(*sources.future)) {
        return {.value = *sources.future, .source = ForwardSource::Future};
    }
    if (sources.perp.has_value() && usable(*sources.perp) && !sources.carry.is_undef()) {
        const auto perp = sources.perp->raw;
        return {.value = FixedE9::from_raw(perp + mul_div(perp, sources.carry.raw, k_e9)),
                .source = ForwardSource::Perp};
    }
    return {};
}

} // namespace raikiri::options
