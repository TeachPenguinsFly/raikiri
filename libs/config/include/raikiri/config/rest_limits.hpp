#pragma once

// The REST half of a venue's limits table, in the shape exec::RateBudget takes.
// Header-only so config itself need not link exec.

#include <raikiri/config/settings.hpp>
#include <raikiri/exec/rate_budget.hpp>

#include <vector>

namespace raikiri::config {

[[nodiscard]] inline std::vector<exec::RateLimit> rest_limits(const VenueSettings& venue) {
    std::vector<exec::RateLimit> out;
    for (const auto& limit : venue.limits) {
        if (limit.kind == "rest") {
            out.push_back({.name = limit.name,
                           .window_ns = limit.window_ms * 1'000'000ULL,
                           .max = limit.max,
                           .reserve = limit.reserve,
                           .match = limit.match});
        }
    }
    return out;
}

[[nodiscard]] inline std::vector<exec::RateCost> rest_costs(const VenueSettings& venue) {
    std::vector<exec::RateCost> out;
    for (const auto& cost : venue.costs) {
        out.push_back({.match = cost.match, .cost = cost.cost});
    }
    return out;
}

} // namespace raikiri::config
