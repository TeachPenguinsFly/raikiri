#pragma once

// The account's own money: what it holds, what mode it holds it under, and
// every row that moved it — of which funding and trading fees are the two this
// process reads.

#include <raikiri/common/time.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::okx {

// One row of GET /api/v5/account/bills.
//
// `type` says what the row IS — "8" funding, "2" a trade — and the two put
// their money in DIFFERENT COLUMNS: a funding row states it in `balChg` and
// leaves `fee` at zero, a trade row the reverse. Reading the wrong column
// books a zero over a real charge, which is the whole reason both are carried.
struct WireBill {
    std::string_view bill_id;
    std::string_view inst_id;
    std::string_view type;
    std::string_view bal_chg;
    std::string_view fee;
    std::string_view trade_id;
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_bills = 256;

struct BillBuffer {
    std::array<WireBill, k_max_bills> bills{};
};

[[nodiscard]] std::expected<std::span<const WireBill>, WireError> parse_bills(std::string_view body,
                                                                              BillBuffer& storage);

// The account mode, from `acctLv` on GET /api/v5/account/config.
//
// It is not a curiosity: OKX computes margin PER CURRENCY in the first two
// modes and ACROSS THE ACCOUNT in the last two, so the same per-currency
// available figure answers "what may I commit" in one pair of modes and
// answers nothing in the other. Unknown is the honest state for a config read
// that failed or named a level this does not know — never assumed to be one
// of the four.
enum class AccountMode : std::uint8_t {
    Unknown = 0,
    Spot = 1,          // acctLv 1
    Futures = 2,       // acctLv 2, formerly single-currency margin
    MultiCurrency = 3, // acctLv 3
    Portfolio = 4,     // acctLv 4
};

[[nodiscard]] std::expected<AccountMode, WireError> parse_account_mode(std::string_view body);

// One currency of GET /api/v5/account/balance, from data[0].details[].
//
// The two totals are NOT interchangeable: `cashBal` is the currency's own
// balance and `eq` is that plus the open pnl, so publishing `eq` as the total
// beside `upl` would count the same money twice.
//
// OKX ships an absent value as an EMPTY STRING rather than omitting the key,
// so an empty view here is the venue declining to answer and not a parse that
// missed the field. `upl` is empty in spot mode, where there is no position to
// carry pnl.
struct WireCurrencyBalance {
    std::string_view ccy;
    std::string_view cash_bal;
    std::string_view avail_bal;
    std::string_view upl;
    std::uint64_t u_time_ms{0};
};

// A row per currency the account has touched, not per currency OKX lists.
inline constexpr std::size_t k_max_currencies = 128;

struct BalanceBuffer {
    std::array<WireCurrencyBalance, k_max_currencies> currencies{};
};

[[nodiscard]] std::expected<std::span<const WireCurrencyBalance>, WireError>
parse_balances(std::string_view body, BalanceBuffer& storage);

} // namespace raikiri::venues::okx
