#pragma once

// A funding charge, split across whoever was carrying the instrument.
//
// THE VENUE CHARGES THE NETTED INSTRUMENT. The book is keyed by (strategy,
// instrument), so where two strategies share one instrument the charge has to
// be pro-rated by the size each carries.
//
// That is an APPROXIMATION and is named as one: the split uses the size held
// NOW, not the size held at the settlement stamp, because a per-strategy
// history at the venue's settlement resolution does not exist. It is the
// closest attribution available, not a derivation.
//
// TWO THINGS IT WILL NOT DO. It will not lose a unit — the remainder goes to
// the last holder, because rounding each share on its own drops cents into
// nobody's book and the ledger then disagrees with the venue's balance
// forever. And it will not drop a charge nobody is holding: that is still
// money that moved on this account, so it lands on the manual book.

#include <raikiri/common/fixed_point.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace raikiri::oms {

// One strategy's position in the charged instrument, as the book holds it now.
struct FundingHolder {
    std::uint16_t strategy_id{0};
    // Signed the way the position runs. The split uses the MAGNITUDE: a short
    // is charged funding exactly as a long is.
    std::int64_t lots{0};
};

struct FundingShare {
    std::uint16_t strategy_id{0};
    FixedE9 amount{};
};

// The shares always sum to `amount` exactly. Empty only when there is nothing
// to book — a zero or undefined charge.
[[nodiscard]] std::vector<FundingShare> split_funding(std::span<const FundingHolder> holders,
                                                      FixedE9 amount,
                                                      std::uint16_t unheld_strategy_id);

} // namespace raikiri::oms
