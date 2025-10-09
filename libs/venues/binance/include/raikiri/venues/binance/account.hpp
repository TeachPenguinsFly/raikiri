#pragma once

// The account's own book: what it holds as margin, and every row that moved
// money — of which funding and trading fees are the two this process reads.

#include <raikiri/venues/binance/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::binance {

// One row of GET /fapi/v2/balance. Binance lists every asset it accepts as
// margin, not the ones the account holds, so most rows are zero across the
// board and the reply is mostly padding — which is a mapping decision, not a
// parsing one, and is made in map_balance.
struct WireBalance {
    std::string_view asset;             // asset
    std::string_view balance;           // wallet total
    std::string_view available_balance; // availableBalance: committable now
    std::string_view cross_unpnl;       // crossUnPnl
    std::uint64_t update_ms{0};         // 0 on an asset never held
};

// Binance answers ~30 rows today and adds one whenever it accepts another
// collateral asset. Sized far past that on purpose: refusing the whole body
// over rows that would have been dropped anyway reads as an account holding
// nothing.
inline constexpr std::size_t k_max_balances = 256;

struct BalanceBuffer {
    std::array<WireBalance, k_max_balances> balances{};
};

[[nodiscard]] std::expected<std::span<const WireBalance>, WireError>
parse_balances(std::string_view body, BalanceBuffer& storage);

// One row of /fapi/v1/income, narrowed to settlements.
//
// `income` is SIGNED THE WAY THE ACCOUNT MOVED — negative was paid away — which
// is already the domain's convention, so nothing here flips it. `tranId` is
// binance's own id for the movement and is unique; it is kept for tracing, not
// for identity, because the socket states the same settlement without one.
struct WireIncome {
    std::string_view symbol;
    std::string_view income_type; // FUNDING_FEE | COMMISSION | REALIZED_PNL | ...
    std::string_view income;
    std::string_view asset;
    std::uint64_t tran_id{0};
    // The trade a COMMISSION row was charged on. Zero on funding rows, which
    // have no trade — and it is what a fee reconciliation keys on, because the
    // fee this process already booked is keyed by the same execution.
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
