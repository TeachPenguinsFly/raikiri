#pragma once

// The account's own money and its own configuration: what it holds, what the
// venue charged it, and the two settings without which the user stream will
// not connect and an order will not route.

#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bingx {

// BingX's own documentation shows this answer both as a bare
// {"listenKey":".."} and wrapped in the usual envelope; both are read, because
// which one arrives decides whether the user stream connects at all.
[[nodiscard]] std::expected<std::string_view, WireError> parse_listen_key(std::string_view body);

// data.dualSidePosition, a STRING "true"/"false" rather than a JSON bool.
// True is hedge mode, where an order must name LONG or SHORT; false is one-way
// mode, where it must name BOTH and BingX refuses the others with 109400.
[[nodiscard]] std::expected<bool, WireError> parse_position_mode(std::string_view body);

// One settlement asset of GET /openApi/swap/v3/user/balance.
//
// `balance` EXCLUDES the open pnl and `equity` includes it, so the two must
// never both be read as the total — that is the same money counted twice.
// `availableMargin` is BingX's own per-asset free figure: the margin already
// committed to positions and resting orders is in usedMargin and freezedMargin
// and is not in this.
struct WireBalance {
    std::string_view asset;
    std::string_view balance;           // wallet total, open pnl excluded
    std::string_view available_margin;  // committable to a new order now
    std::string_view unrealized_profit; // on BingX's marks, not ours
};

// A row per settlement asset, not per asset BingX lists. Sized far past the
// handful its USD-M tree settles in: refusing the whole body over rows past
// the end reads as an account holding nothing.
inline constexpr std::size_t k_max_balances = 128;

struct BalanceBuffer {
    std::array<WireBalance, k_max_balances> balances{};
};

// `data` is a bare array here. The v2 spelling of this path answers with a
// single object under `data.balance` instead, and reading one as the other
// finds no rows at all rather than failing.
[[nodiscard]] std::expected<std::span<const WireBalance>, WireError>
parse_balances(std::string_view body, BalanceBuffer& storage);

// One row of GET /openApi/swap/v2/user/income, the account's own book.
//
// `income` is SIGNED THE WAY THE ACCOUNT MOVED — negative was paid away —
// which is already the domain's convention, so nothing flips it. A mapper that
// took the magnitude would report every funding payment as income.
//
// `tranId` and `tradeId` are STRINGS here and carry underscores, so neither
// fits a numeric read: BingX writes them as `170...2_3...9_20...97`.
struct WireIncome {
    std::string_view symbol;
    std::string_view income_type; // FUNDING_FEE | TRADING_FEE | REALIZED_PNL | ...
    std::string_view income;
    std::string_view asset;
    std::string_view tran_id;
    std::string_view trade_id; // the trade a TRADING_FEE row was charged on
    std::uint64_t time_ms{0};
};

// Two hundred rows are asked for; the slack is what keeps a venue that answers
// with more from costing the whole book.
inline constexpr std::size_t k_max_income_rows = 256;

struct IncomeBuffer {
    std::array<WireIncome, k_max_income_rows> rows{};
};

[[nodiscard]] std::expected<std::span<const WireIncome>, WireError>
parse_income(std::string_view body, IncomeBuffer& storage);

} // namespace raikiri::venues::bingx
