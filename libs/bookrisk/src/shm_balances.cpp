#include <raikiri/bookrisk/shm_balances.hpp>
#include <raikiri/bookrisk/value.hpp>

#include <algorithm>

namespace raikiri::bookrisk {
namespace internal {

[[nodiscard]] FixedE9 rate_of(const AssetCode& asset, std::span<const AccountChannels> accounts) noexcept {
    for (const auto& account : accounts) {
        if (account.channels == nullptr || account.channels->catalogue == nullptr || account.channels->bbo == nullptr) {
            continue;
        }
        for (const auto& listed : account.channels->catalogue->all()) {
            const auto& base = base_of(listed);
            const auto market = base.id.market();
            if ((market != MarketType::Spot && market != MarketType::Perp) || base.underlying.view() != asset.view()) {
                continue;
            }
            const auto symbol = base.local_symbol.view();
            const auto slash = symbol.find('/');
            if (slash == std::string_view::npos || !is_dollar(symbol.substr(slash + 1))) {
                continue;
            }
            Bbo quote{};
            const auto idx = base.id.symbol().raw;
            if (idx >= shm::k_bbo_cells || account.channels->bbo->read(idx, quote) == 0 || quote.bid.is_undef() ||
                quote.ask.is_undef()) {
                continue;
            }
            return base.ticks_to_price(PriceTicks::from_raw((quote.bid.raw + quote.ask.raw) / 2));
        }
    }
    return FixedE9::undef();
}

} // namespace internal

BalanceReading read_balances(std::span<const AccountChannels> accounts) {
    BalanceTally tally;
    for (const auto& account : accounts) {
        if (account.channels == nullptr || account.channels->balances == nullptr) {
            continue;
        }
        shm::BalanceSnapshot snapshot{};
        if (account.channels->balances->read(0, snapshot) == 0 || snapshot.count == 0) {
            continue;
        }
        tally.add_account();
        for (std::uint32_t i = 0; i < std::min<std::uint32_t>(snapshot.count, shm::k_balance_rows); ++i) {
            const auto& row = snapshot.rows[i];
            tally.add(account.venue,
                      row,
                      is_dollar(row.asset.view()) ? FixedE9::undef() : internal::rate_of(row.asset, accounts));
        }
    }
    return tally.reading();
}

} // namespace raikiri::bookrisk
