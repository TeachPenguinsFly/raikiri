#pragma once

#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bingx {

// bingx sends this both bare and inside the envelope; both are read
[[nodiscard]] std::expected<std::string_view, WireError> parse_listen_key(std::string_view body);

// a string "true"/"false"; true is hedge mode (long/short), false one-way (both)
[[nodiscard]] std::expected<bool, WireError> parse_position_mode(std::string_view body);

// balance excludes open pnl and equity includes it; never sum them
struct WireBalance {
    std::string_view asset;
    std::string_view balance;           // wallet total, open pnl excluded
    std::string_view available_margin;  // committable to a new order now
    std::string_view unrealized_profit; // on the venue's marks
};

inline constexpr std::size_t k_max_balances = 128;

struct BalanceBuffer {
    std::array<WireBalance, k_max_balances> balances{};
};

// data is a bare array here; v2 returns a single object under data.balance
[[nodiscard]] std::expected<std::span<const WireBalance>, WireError>
parse_balances(std::string_view body, BalanceBuffer& storage);

// income is signed as the account moved; tranId and tradeId are underscored strings
struct WireIncome {
    std::string_view symbol;
    std::string_view income_type; // FUNDING_FEE | TRADING_FEE | REALIZED_PNL | ...
    std::string_view income;
    std::string_view asset;
    std::string_view tran_id;
    std::string_view trade_id; // the trade a TRADING_FEE row was charged on
    std::uint64_t time_ms{0};
};

inline constexpr std::size_t k_max_income_rows = 256;

struct IncomeBuffer {
    std::array<WireIncome, k_max_income_rows> rows{};
};

[[nodiscard]] std::expected<std::span<const WireIncome>, WireError>
parse_income(std::string_view body, IncomeBuffer& storage);

} // namespace raikiri::venues::bingx
