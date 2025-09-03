#include <raikiri/exec/rate_budget.hpp>

namespace raikiri::exec {
namespace {

// A bucket the operator left at zero capacity, or gave a capacity but no
// window, is DISABLED rather than instantaneous. A window of zero would refill
// infinitely fast and silently limit nothing while looking configured.
[[nodiscard]] bool configured(const RateBudget::BucketConfig& cfg) noexcept {
    return cfg.capacity != 0 && cfg.window_ns != 0;
}

[[nodiscard]] RateLimiter make(const RateBudget::BucketConfig& cfg) noexcept {
    return configured(cfg) ? RateLimiter{cfg.capacity, cfg.window_ns, cfg.reserve}
                           : RateLimiter{0, 0, 0};
}

} // namespace

RateBudget::RateBudget(const Table& table, Costs costs) noexcept
    : buckets_{make(table[0]), make(table[1]), make(table[2]), make(table[3])},
      limited_{
          configured(table[0]), configured(table[1]), configured(table[2]), configured(table[3])},
      costs_{costs} {}

std::expected<void, RateDenied>
RateBudget::take(RateBucket bucket, std::uint32_t cost, MonoNanos now, RateClass cls) noexcept {
    const auto slot = static_cast<std::size_t>(bucket);
    if (!limited_[slot]) {
        return {};
    }
    const std::lock_guard lock{mu_};
    return buckets_[slot].take(cost, now, cls);
}

void RateBudget::penalize(RateBucket bucket, MonoNanos now, std::uint64_t backoff_ns) noexcept {
    const std::lock_guard lock{mu_};
    buckets_[static_cast<std::size_t>(bucket)].penalize(now, backoff_ns);
}

std::uint64_t RateBudget::denials(RateBucket bucket) noexcept {
    const std::lock_guard lock{mu_};
    return buckets_[static_cast<std::size_t>(bucket)].denials();
}

} // namespace raikiri::exec
