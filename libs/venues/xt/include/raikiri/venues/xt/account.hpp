#pragma once

#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::xt {

// availableBalance is already net of frozen and position margin; do not subtract them again
struct WireFuturesBalance {
    std::string_view coin; // lowercase: `usdt`, not `USDT`
    std::string_view wallet_balance;
    std::string_view available_balance;
};

inline constexpr std::size_t k_max_balances = 512;

struct FuturesBalanceBuffer {
    std::array<WireFuturesBalance, k_max_balances> balances{};
};

// result is a bare array; decimals arrive quoted or bare, so raw tokens are kept
[[nodiscard]] std::expected<std::span<const WireFuturesBalance>, WireError>
parse_futures_balances(std::string_view body, FuturesBalanceBuffer& storage);

// amount is already signed and side only cross-checks it; FUND and FEE are booked, CLOSE_POSITION is pnl the fill booked
struct WireBill {
    std::string_view id;
    std::string_view symbol; // lowercase, as the catalogue spells it
    std::string_view coin;
    std::string_view type;
    std::string_view amount;
    std::string_view side;
    std::uint64_t created_ms{0};
};

inline constexpr std::size_t k_max_bills = 512;

struct BillBuffer {
    std::array<WireBill, k_max_bills> bills{};
};

[[nodiscard]] std::expected<std::span<const WireBill>, WireError> parse_bills(std::string_view body,
                                                                              BillBuffer& storage);

} // namespace raikiri::venues::xt
