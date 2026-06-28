#include <raikiri/exec/rate_budget.hpp>

#include <algorithm>
#include <utility>

namespace raikiri::exec {
namespace {

[[nodiscard]] bool query_has(std::string_view query, std::string_view pair) noexcept {
    while (!query.empty()) {
        const auto amp = query.find('&');
        if (query.substr(0, amp) == pair) {
            return true;
        }
        if (amp == std::string_view::npos) {
            break;
        }
        query.remove_prefix(amp + 1);
    }
    return false;
}

} // namespace

bool path_matches(std::string_view rule, std::string_view target) noexcept {
    const auto rq = rule.find('?');
    const auto tq = target.find('?');
    if (!target.substr(0, tq).starts_with(rule.substr(0, rq))) {
        return false;
    }
    if (rq == std::string_view::npos) {
        return true;
    }
    const auto query = tq == std::string_view::npos ? std::string_view{} : target.substr(tq + 1);
    auto wanted = rule.substr(rq + 1);
    while (!wanted.empty()) {
        const auto amp = wanted.find('&');
        if (!query_has(query, wanted.substr(0, amp))) {
            return false;
        }
        if (amp == std::string_view::npos) {
            break;
        }
        wanted.remove_prefix(amp + 1);
    }
    return true;
}

RateBudget::RateBudget(std::vector<RateLimit> limits,
                       std::vector<RateCost> costs,
                       std::uint32_t default_cost)
    : costs_{std::move(costs)}, default_cost_{default_cost} {
    slots_.reserve(limits.size());
    for (auto& limit : limits) {
        SlidingLog log{limit.max, limit.window_ns, limit.reserve};
        slots_.push_back(Slot{.spec = std::move(limit), .log = std::move(log)});
    }
}

bool RateBudget::meters(const RateLimit& limit, std::string_view target) noexcept {
    return limit.match.empty() || std::ranges::any_of(limit.match, [target](const auto& rule) {
               return path_matches(rule, target);
           });
}

std::uint32_t RateBudget::cost(std::string_view target) const noexcept {
    const RateCost* best = nullptr;
    for (const auto& rule : costs_) {
        if (path_matches(rule.match, target) &&
            (best == nullptr || rule.match.size() > best->match.size())) {
            best = &rule;
        }
    }
    return best != nullptr ? best->cost : default_cost_;
}

std::expected<void, RateDenied>
RateBudget::take(std::string_view target, MonoNanos now, RateClass cls) noexcept {
    const auto units = cost(target);
    if (units == 0 || slots_.empty()) {
        return {};
    }
    const bool may_use_reserve = cls == RateClass::Order;
    const std::lock_guard lock{mu_};
    std::uint64_t wait = 0;
    for (auto& slot : slots_) {
        if (meters(slot.spec, target)) {
            wait = std::max(wait, slot.log.wait_ns(units, now.ns, may_use_reserve));
        }
    }
    if (wait != 0) {
        ++denials_;
        return std::unexpected{RateDenied{.retry_after_ns = wait}};
    }
    for (auto& slot : slots_) {
        if (meters(slot.spec, target)) {
            slot.log.spend(units, now.ns);
        }
    }
    return {};
}

void RateBudget::penalize(std::string_view target, MonoNanos now, std::uint64_t backoff_ns) noexcept {
    const std::lock_guard lock{mu_};
    for (auto& slot : slots_) {
        if (meters(slot.spec, target)) {
            slot.log.hold(now.ns, backoff_ns);
        }
    }
}

void RateBudget::penalize_all(MonoNanos now, std::uint64_t backoff_ns) noexcept {
    const std::lock_guard lock{mu_};
    for (auto& slot : slots_) {
        slot.log.hold(now.ns, backoff_ns);
    }
}

std::uint64_t RateBudget::denials() noexcept {
    const std::lock_guard lock{mu_};
    return denials_;
}

} // namespace raikiri::exec
