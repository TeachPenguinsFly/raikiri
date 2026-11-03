#include <raikiri/oms/funding_split.hpp>

#include <algorithm>
#include <cstdlib>
#include <iterator>

namespace raikiri::oms {

std::vector<FundingShare>
split_funding(std::span<const FundingHolder> holders, FixedE9 amount, std::uint16_t unheld_strategy_id) {
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
        // the remainder goes to the last holder so the parts sum exactly
        const auto share = &held == last ? amount.raw - paid : amount.raw * std::abs(held.lots) / total_lots;
        paid += share;
        shares.push_back({.strategy_id = held.strategy_id, .amount = FixedE9{share}});
    }
    return shares;
}

std::vector<FundingShare>
split_funding_net(std::span<const FundingHolder> holders, FixedE9 amount, std::uint16_t unheld_strategy_id) {
    if (amount.raw == 0 || amount.is_undef()) {
        return {};
    }
    std::int64_t net = 0;
    const FundingHolder* last = nullptr;
    for (const auto& held : holders) {
        if (held.lots != 0) {
            net += held.lots;
            last = &held;
        }
    }
    if (net == 0) {
        return {{.strategy_id = unheld_strategy_id, .amount = amount}};
    }
    std::vector<FundingShare> shares;
    std::int64_t paid = 0;
    for (const auto& held : holders) {
        if (held.lots == 0) {
            continue;
        }
        const auto share = &held == last ? amount.raw - paid : mul_div(amount.raw, held.lots, net);
        paid += share;
        shares.push_back({.strategy_id = held.strategy_id, .amount = FixedE9{share}});
    }
    return shares;
}

std::vector<FundingShare> split_funding_flat(std::span<const FundingHolder> holders, FixedE9 long_side) {
    std::vector<FundingHolder> longs;
    std::vector<FundingHolder> shorts;
    std::int64_t net = 0;
    for (const auto& held : holders) {
        net += held.lots;
        if (held.lots > 0) {
            longs.push_back(held);
        } else if (held.lots < 0) {
            shorts.push_back(held);
        }
    }
    if (net != 0 || longs.empty()) {
        return {};
    }
    auto shares = split_funding(longs, long_side, 0);
    std::ranges::move(split_funding(shorts, FixedE9{-long_side.raw}, 0), std::back_inserter(shares));
    return shares;
}

} // namespace raikiri::oms
