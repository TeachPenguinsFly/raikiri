#include <raikiri/bookrisk/value.hpp>
#include <raikiri/portfolio/pnl.hpp>

#include <algorithm>

namespace raikiri::bookrisk {
namespace internal {

[[nodiscard]] FixedE9 times(FixedE9 amount, FixedE9 rate) noexcept {
    return turnover_e9(amount, rate);
}

[[nodiscard]] std::string_view quote_of(std::string_view symbol) noexcept {
    const auto slash = symbol.find('/');
    if (slash == std::string_view::npos) {
        return {};
    }
    const auto tail = symbol.substr(slash + 1);
    return tail.substr(0, tail.find_first_of(":-"));
}

} // namespace internal

bool is_dollar(std::string_view asset) noexcept {
    return asset.starts_with("USD") || asset == "DAI" || asset == "FDUSD" || asset == "TUSD" || asset == "PYUSD";
}

void add_position(PnlReading& into, const Instrument& listed, const portfolio::Position& position, Mark mark) noexcept {
    const auto& base = base_of(listed);
    const bool held = !position.flat();
    if (held) {
        ++into.positions;
    }
    const bool option = base.id.market() == MarketType::Option;
    const auto in_base = portfolio::settle_side(listed) == portfolio::SettleSide::Base;
    if (!in_base && !option) {
        const auto quote = internal::quote_of(base.local_symbol.view());
        if (!quote.empty() && !is_dollar(quote)) {
            ++into.unvalued;
            return;
        }
    }
    if (!held && position.net_realized().raw == 0) {
        return;
    }
    const bool has_mid = !mark.mid.is_undef() && mark.mid.raw > 0;
    if (held && !has_mid) {
        ++into.unmarked;
        return;
    }
    auto rate = FixedE9::from_raw(k_e9);
    if (in_base) {
        rate = option ? mark.underlying : has_mid ? base.ticks_to_price(mark.mid) : FixedE9::undef();
        if (rate.is_undef() || rate.raw <= 0) {
            ++into.unmarked;
            return;
        }
    }
    const auto realized = internal::times(position.net_realized(), rate);
    const auto open = held ? portfolio::unrealized_pnl(base, position, mark.mid) : FixedE9::from_raw(0);
    const auto unrealized = internal::times(open, rate);
    if (realized.is_undef() || unrealized.is_undef()) {
        ++into.unmarked;
        return;
    }
    into.realized = FixedE9::from_raw(into.realized.raw + realized.raw);
    into.unrealized = FixedE9::from_raw(into.unrealized.raw + unrealized.raw);
}

void BalanceTally::add(VenueId venue, const BalanceReport& row, FixedE9 rate) {
    if (row.total.is_undef() || row.total.raw == 0) {
        return;
    }
    if (row.wallet == WalletKind::Unified) {
        const auto same = [&](const Seen& seen) { return seen.venue == venue && seen.asset == row.asset; };
        if (std::ranges::any_of(unified_, same)) {
            return;
        }
        unified_.push_back({.venue = venue, .asset = row.asset});
    }
    const auto value = is_dollar(row.asset.view()) ? row.total : turnover_e9(row.total, rate);
    if (value.is_undef()) {
        ++reading_.unvalued;
        return;
    }
    reading_.dollars = FixedE9::from_raw(reading_.dollars.raw + value.raw);
}

} // namespace raikiri::bookrisk
