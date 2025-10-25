#pragma once

// The account's own money: what it holds, and what moved through it.
//
// FUNDING IS A LEDGER LINE HERE, not an execution: WEEX settles it into the
// collateral balance and records it in the same bill list as a deposit or a
// transfer, under `incomeType` "position_funding". That is why the filter is
// the row's type and not its endpoint — the bills read answers everything the
// account did, and booking the wrong rows as funding books a withdrawal as a
// settlement.

#include <raikiri/venues/weex/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::weex {

// One coin of GET /capi/v3/account/balance.
struct WireBalance {
    std::string_view asset;
    std::string_view balance;           // the venue's total for this coin
    std::string_view available_balance; // what a new order may be placed against
    std::string_view unrealize_pnl;     // on WEEX's own marks, never recomputed
};

// A contract account holds its collateral in a handful of coins, not the
// hundreds a spot wallet lists.
inline constexpr std::size_t k_max_balances = 64;

struct BalanceBuffer {
    std::array<WireBalance, k_max_balances> balances{};
};

// A BARE ARRAY, with no parameters and no envelope.
[[nodiscard]] std::expected<std::span<const WireBalance>, WireError>
parse_balances(std::string_view body, BalanceBuffer& storage);

// One row of POST /capi/v3/account/income.
//
// `bill_id` IS THE IDENTITY. WEEX mints one per row, which is what lets a
// settlement be booked exactly once — most venues mint nothing for funding, and
// the ledger downstream keys on this.
// `asset` and `balance` ride the row too and are not read: a LedgerEntry
// settles in the instrument's own currency, which the instrument already names.
struct WireBill {
    std::string_view bill_id;
    std::string_view symbol;
    // SIGNED THE WAY THE ACCOUNT MOVED: the venue calls this a collateral
    // change, so a settlement received is positive and one paid away negative.
    // It is already the convention the ledger wants, which is why nothing here
    // flips it.
    std::string_view income;
    std::string_view income_type; // position_funding, transfer_in, deposit, ...
    std::uint64_t time_ms{0};
};

// The venue caps a page at 100 rows.
inline constexpr std::size_t k_max_bills = 128;

struct BillBuffer {
    std::array<WireBill, k_max_bills> bills{};
};

// The rows sit under `items`, beside the cursor that would fetch the next page.
//
// THE CURSOR IS NOT READ. One page is what a reconciliation pass asks for, and
// a pager nothing calls is a facility nobody exercises; a caller that needs
// more moves its `since` forward instead.
[[nodiscard]] std::expected<std::span<const WireBill>, WireError> parse_bills(std::string_view body,
                                                                              BillBuffer& storage);

// The one `incomeType` that is a funding settlement.
[[nodiscard]] bool is_funding_bill(const WireBill& wire) noexcept;

} // namespace raikiri::venues::weex
