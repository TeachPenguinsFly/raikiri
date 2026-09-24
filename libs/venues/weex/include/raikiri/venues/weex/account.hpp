#pragma once

// funding settles as a bill row with incomeType position_funding

#include <raikiri/venues/weex/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::weex {

// one coin of GET /capi/v3/account/balance
struct WireBalance {
    std::string_view asset;
    std::string_view balance;           // the venue's total for this coin
    std::string_view available_balance; // what a new order may be placed against
    std::string_view unrealize_pnl; // on weex's own marks
};

inline constexpr std::size_t k_max_balances = 64;

struct BalanceBuffer {
    std::array<WireBalance, k_max_balances> balances{};
};

// bare array, no parameters, no envelope
[[nodiscard]] std::expected<std::span<const WireBalance>, WireError>
parse_balances(std::string_view body, BalanceBuffer& storage);

// bill_id is the identity the ledger keys on
struct WireBill {
    std::string_view bill_id;
    std::string_view symbol;
    // signed as the account moved: received positive, paid negative
    std::string_view income;
    std::string_view income_type; // position_funding, transfer_in, deposit, ...
    std::uint64_t time_ms{0};
};

// the venue caps a page at 100 rows
inline constexpr std::size_t k_max_bills = 128;

struct BillBuffer {
    std::array<WireBill, k_max_bills> bills{};
};

// rows sit under items; the cursor is not read
[[nodiscard]] std::expected<std::span<const WireBill>, WireError> parse_bills(std::string_view body,
                                                                              BillBuffer& storage);

// the incomeType that is a funding settlement
[[nodiscard]] bool is_funding_bill(const WireBill& wire) noexcept;

} // namespace raikiri::venues::weex
