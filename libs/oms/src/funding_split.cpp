#include <raikiri/oms/funding_split.hpp>

#include <cstdlib>

namespace raikiri::oms {

std::vector<FundingShare> split_funding(std::span<const FundingHolder> holders,
                                        FixedE9 amount,
                                        std::uint16_t unheld_strategy_id) {
    if (amount.raw == 0 || amount.is_undef()) {
        return {};
    }
    std::int64_t total_lots = 0;
    const FundingHolder* last = nullptr;
    for (const auto& held : holders) {
        if (held.lots == 0) {
            continue;
        }
        total_lots += std::abs(held.lots);
        last = &held;
    }
    if (total_lots == 0) {
        return {{.strategy_id = unheld_strategy_id, .amount = amount}};
    }

    std::vector<FundingShare> shares;
    std::int64_t paid = 0;
    for (const auto& held : holders) {
        if (held.lots == 0) {
            continue;
        }
        // The remainder goes to the last holder so the parts sum to the charge
        // exactly; rounding each share on its own loses units into nobody's
        // book, and the ledger then disagrees with the venue's balance forever.
        const auto share =
            &held == last ? amount.raw - paid : amount.raw * std::abs(held.lots) / total_lots;
        paid += share;
        shares.push_back({.strategy_id = held.strategy_id, .amount = FixedE9{share}});
    }
    return shares;
}

} // namespace raikiri::oms
