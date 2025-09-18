#pragma once

// The account's own book: what the futures wallet holds, one coin per row.

#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::xt {

// One coin of the futures wallet. XT states no PnL and no timestamp on this
// body at all, and the margin it freezes — openOrderMarginFrozen,
// isolatedMargin, crossedMargin — is left on the wire: availableBalance is
// already net of all three, and subtracting them again downstream would charge
// the account for the same margin twice.
struct WireFuturesBalance {
    std::string_view coin; // lowercase: `usdt`, not `USDT`
    std::string_view wallet_balance;
    std::string_view available_balance;
};

// The wallet lists one row per collateral coin, far fewer than a page of
// orders — but sized alike so a body XT lengthens is never refused.
//
// Its own, though it matches the order buffer's: both used to borrow one
// k_max_private_records, which made a change to either silently resize the
// other and the position buffer beside them.
inline constexpr std::size_t k_max_balances = 512;

struct FuturesBalanceBuffer {
    std::array<WireFuturesBalance, k_max_balances> balances{};
};

// /future/user/v1/balance/list, whose `result` is a bare ARRAY. The decimals
// arrive in both encodings — quoted in a live body, bare numbers in XT's own
// doc example — so each is kept as its raw token, like every other private
// decimal here.
[[nodiscard]] std::expected<std::span<const WireFuturesBalance>, WireError>
parse_futures_balances(std::string_view body, FuturesBalanceBuffer& storage);

// One row of the futures wallet's own ledger, off
// /future/user/v1/balance/bills.
//
// `type` says what the row IS. Measured over 100 live rows: FUND (95), FEE (3)
// and CLOSE_POSITION (2). Only the first two are money this process did not
// already book — CLOSE_POSITION is realised PnL, which the fill that closed
// the position already accounted for, and booking it again double-counts.
//
// `amount` IS ALREADY SIGNED and `side` merely restates it: over those same
// 100 rows every ADD carried a positive amount and every SUB a negative one,
// with no disagreement. Re-applying `side` to the value would flip every SUB
// row — checked against the running `afterAmount` balance, that rule matched
// 4 of 19 consecutive pairs where taking the amount verbatim matched 10.
// `side` is therefore read as a cross-check and never as the sign.
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

// The `result` here is an object holding `items`, like every other paged
// private body on this venue.
[[nodiscard]] std::expected<std::span<const WireBill>, WireError> parse_bills(std::string_view body,
                                                                              BillBuffer& storage);

} // namespace raikiri::venues::xt
