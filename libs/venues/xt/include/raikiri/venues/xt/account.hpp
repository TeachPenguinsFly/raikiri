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

} // namespace raikiri::venues::xt
