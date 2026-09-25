#pragma once

#include <raikiri/common/time.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::okx {

// type 8 is funding and books into balChg, type 2 is a trade and books into fee
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

// okx margins per currency in modes 1-2 and across the account in 3-4; unknown when the read failed
enum class AccountMode : std::uint8_t {
    Unknown = 0,
    Spot = 1,          // acctLv 1
    Futures = 2,       // acctLv 2, formerly single-currency margin
    MultiCurrency = 3, // acctLv 3
    Portfolio = 4,     // acctLv 4
};

[[nodiscard]] std::expected<AccountMode, WireError> parse_account_mode(std::string_view body);

// cashBal is the currency's own balance and eq adds open pnl; absent values arrive as empty strings
struct WireCurrencyBalance {
    std::string_view ccy;
    std::string_view cash_bal;
    std::string_view avail_bal;
    std::string_view upl;
    std::uint64_t u_time_ms{0};
};

// a row per currency the account has touched
inline constexpr std::size_t k_max_currencies = 128;

struct BalanceBuffer {
    std::array<WireCurrencyBalance, k_max_currencies> currencies{};
};

[[nodiscard]] std::expected<std::span<const WireCurrencyBalance>, WireError> parse_balances(std::string_view body,
                                                                                            BalanceBuffer& storage);

} // namespace raikiri::venues::okx
