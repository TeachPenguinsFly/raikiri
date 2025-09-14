#pragma once

// The account's own book: what the wallet holds, one currency at a time.

#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <expected>
#include <string_view>

namespace raikiri::venues::kucoin {

// GET /api/v1/account-overview?currency=X — the futures wallet, one currency
// at a time. Every figure is a JSON NUMBER rendered by Java, so a balance
// arrives with more fractional digits than FixedE9 models; the mapper reads
// them as amounts, not as exact decimals. The body carries no timestamp.
//
// `accountEquity` is deliberately absent: KuCoin defines it as marginBalance
// plus unrealisedPNL, both of which are here, so carrying it would be carrying
// the same money twice under a third name.
struct WireAccountOverview {
    std::string_view currency;
    std::string_view margin_balance;    // the wallet, EXCLUDING unrealised pnl
    std::string_view available_balance; // what a new order may commit
    std::string_view unrealised_pnl;
};

[[nodiscard]] std::expected<WireAccountOverview, WireError>
parse_account_overview(std::string_view body);

} // namespace raikiri::venues::kucoin
