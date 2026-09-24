#pragma once

// one venue's rest limits from config/venues.yaml; locked, since all clients share the per-ip allowance

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

    // the most specific matching cost rule, else the default
    [[nodiscard]] std::uint32_t cost(std::string_view target) const noexcept;

    [[nodiscard]] std::expected<void, RateDenied>
    take(std::string_view target, MonoNanos now, RateClass cls) noexcept;

    // the venue refused: hold every limit the target spends from
    void penalize(std::string_view target, MonoNanos now, std::uint64_t backoff_ns) noexcept;
    // an ip-wide refusal holds every limit
    void penalize_all(MonoNanos now, std::uint64_t backoff_ns) noexcept;

    [[nodiscard]] std::uint64_t denials() noexcept;

    struct Usage {
        std::string name;
        std::uint32_t max{0};
        std::uint64_t window_ns{0};
        std::uint64_t calls{0};
        std::uint64_t denied{0};
    };
    [[nodiscard]] std::vector<Usage> usage();

private:
    struct Slot {
        RateLimit spec;
        SlidingLog log;
        std::uint64_t calls{0};
        std::uint64_t denied{0};
    };

    [[nodiscard]] static bool meters(const RateLimit& limit, std::string_view target) noexcept;

    std::mutex mu_;
    std::vector<Slot> slots_;
    std::vector<RateCost> costs_;
    std::uint32_t default_cost_{1};
    std::uint64_t denials_{0};
};

// the admit gate a market-data rest client asks before each send; a null budget is unlimited
[[nodiscard]] inline std::function<bool(std::string_view)> market_data_gate(RateBudget* budget,
                                                                           const Clock& clock) {
    return [budget, clock = &clock](std::string_view target) {
        return budget == nullptr ||
               budget->take(target, clock->mono(), RateClass::Poll).has_value();
    };
}

} // namespace raikiri::exec
