#pragma once

// pro-rates a funding charge by current size; the remainder goes to the last holder, unheld
// charges to the manual book

#include <raikiri/common/fixed_point.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace raikiri::oms {

// One strategy's position in the charged instrument, as the book holds it now.
struct FundingHolder {
    std::uint16_t strategy_id{0};
    // signed; the split uses the magnitude
    std::int64_t lots{0};
};

struct FundingShare {
    std::uint16_t strategy_id{0};
    FixedE9 amount{};
};

// shares sum to `amount` exactly; empty for a zero or undefined charge
[[nodiscard]] std::vector<FundingShare>
split_funding(std::span<const FundingHolder> holders, FixedE9 amount, std::uint16_t unheld_strategy_id);

// the venue charged the account's net, while books crossed inside the desk hold both sides of it: each book owes what
// the rate would charge its own size, so a short earns what a long pays. shares sum to `amount` exactly; with a net of
// zero there is no rate to read from the charge, and it falls back to split_funding
[[nodiscard]] std::vector<FundingShare>
split_funding_net(std::span<const FundingHolder> holders, FixedE9 amount, std::uint16_t unheld_strategy_id);

} // namespace raikiri::oms
