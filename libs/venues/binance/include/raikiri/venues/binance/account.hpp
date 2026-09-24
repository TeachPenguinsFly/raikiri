#pragma once

#include <raikiri/venues/binance/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::binance {

// binance lists every margin asset it accepts, so most rows are zero; map_balance drops them
struct WireAssetBalance {
    std::string_view asset;
    std::string_view wallet_balance;    // walletBalance: the total, pnl excluded
    std::string_view unrealized_profit; // unrealizedProfit: cross and isolated
    // availableBalance: per-asset only in single-asset mode, see WireAccount
    std::string_view available_balance;
    std::uint64_t update_ms{0}; // 0 on an asset never held
};

// sized well past the ~30 rows binance returns so a new collateral asset does not fail the parse
inline constexpr std::size_t k_max_balances = 256;

struct BalanceBuffer {
    std::array<WireAssetBalance, k_max_balances> balances{};
};

// multi_assets_margin defaults to true so an unread flag withholds the per-asset available figure
struct WireAccount {
    bool multi_assets_margin{true};
    std::span<const WireAssetBalance> assets;
};

[[nodiscard]] std::expected<WireAccount, WireError> parse_account(std::string_view body,
                                                                  BalanceBuffer& storage);

// one row of /fapi/v1/income; income is signed as the account moved
struct WireIncome {
    std::string_view symbol;
    std::string_view income_type; // FUNDING_FEE | COMMISSION | REALIZED_PNL | ...
    std::string_view income;
    std::string_view asset;
    std::uint64_t tran_id{0};
    // the trade a commission row was charged on; zero on funding rows
    std::uint64_t trade_id{0};
    std::uint64_t time_ms{0};
};

inline constexpr std::size_t k_max_income_rows = 256;

struct IncomeBuffer {
    std::array<WireIncome, k_max_income_rows> rows{};
};

[[nodiscard]] std::expected<std::span<const WireIncome>, WireError>
parse_income(std::string_view body, IncomeBuffer& storage);

} // namespace raikiri::venues::binance
