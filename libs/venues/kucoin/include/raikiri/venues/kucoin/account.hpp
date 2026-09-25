#pragma once

#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <expected>
#include <string_view>

namespace raikiri::venues::kucoin {

// numbers carry more digits than FixedE9; accountEquity is omitted as marginBalance plus unrealisedPNL
struct WireAccountOverview {
    std::string_view currency;
    std::string_view margin_balance;    // excludes unrealised pnl
    std::string_view available_balance; // what a new order may commit
    std::string_view unrealised_pnl;
};

[[nodiscard]] std::expected<WireAccountOverview, WireError> parse_account_overview(std::string_view body);

} // namespace raikiri::venues::kucoin
