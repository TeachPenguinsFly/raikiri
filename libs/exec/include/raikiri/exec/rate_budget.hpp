#pragma once

// One venue's REST limits, as config/venues.yaml declares them: named sliding
// windows, each metering the paths it matches, and per-path costs. A request
// spends from every limit that matches it, or from none if any would refuse.
//
// SHARED, AND THEREFORE LOCKED: the order path, the account poller and the
// market-data pools spend one per-IP allowance together.

#include <raikiri/common/sliding_log.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/exec/order_gate.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::exec {

struct RateLimit {
    std::string name;
    std::uint64_t window_ns{0};
    std::uint32_t max{0};
    std::uint32_t reserve{0};       // only RateClass::Order may spend it
    std::vector<std::string> match; // path rules; empty meters every request
};

struct RateCost {
    std::string match;
    std::uint32_t cost{1};
};

// `rule` is a path prefix, optionally followed by `?k=v&...` pairs that must
// each appear in the target's query.
[[nodiscard]] bool path_matches(std::string_view rule, std::string_view target) noexcept;

class RateBudget {
public:
    RateBudget(std::vector<RateLimit> limits,
               std::vector<RateCost> costs,
               std::uint32_t default_cost = 1);

    // The most specific cost rule that matches, else the default.
    [[nodiscard]] std::uint32_t cost(std::string_view target) const noexcept;

    [[nodiscard]] std::expected<void, RateDenied>
    take(std::string_view target, MonoNanos now, RateClass cls) noexcept;

    // The venue refused: hold every limit the target spends from.
    void penalize(std::string_view target, MonoNanos now, std::uint64_t backoff_ns) noexcept;
    // An IP-wide refusal (a ban) holds every limit.
    void penalize_all(MonoNanos now, std::uint64_t backoff_ns) noexcept;

    [[nodiscard]] std::uint64_t denials() noexcept;

private:
    struct Slot {
        RateLimit spec;
        SlidingLog log;
    };

    [[nodiscard]] static bool meters(const RateLimit& limit, std::string_view target) noexcept;

    std::mutex mu_;
    std::vector<Slot> slots_;
    std::vector<RateCost> costs_;
    std::uint32_t default_cost_{1};
    std::uint64_t denials_{0};
};

// The admit gate a market-data REST client asks before each send. No budget
// is unlimited.
[[nodiscard]] inline std::function<bool(std::string_view)> market_data_gate(RateBudget* budget,
                                                                           const Clock& clock) {
    return [budget, clock = &clock](std::string_view target) {
        return budget == nullptr ||
               budget->take(target, clock->mono(), RateClass::Poll).has_value();
    };
}

} // namespace raikiri::exec
