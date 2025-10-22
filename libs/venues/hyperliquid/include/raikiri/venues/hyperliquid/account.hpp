#pragma once

// The account's own record of what funding cost it, off /info
// {"type":"userFunding"}.

#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::hyperliquid {

// One settlement, flattened out of the row's two levels: the stamp sits at the
// top and the money under `delta`.
//
// `usdc` IS ALREADY SIGNED — negative was paid away — and `szi` beside it
// merely restates which way the position pointed. Re-deriving the sign from
// `szi` and `fundingRate` would disagree with the venue on every row it
// rounded, and this is the number its books hold.
//
// `type` is read rather than assumed: the sibling ledger call
// (`userNonFundingLedgerUpdates`) shares this row shape with deposits,
// withdrawals and transfers, and a reader that trusted the endpoint instead of
// the field would book a deposit as income the moment the two are merged.
//
// The row's `hash` is not carried. MEASURED over 500 consecutive rows it is
// 0x000…0 on every funding delta — the non-funding ledger's rows do carry real
// ones — so it identifies nothing, and at 66 characters it would not fit an
// ExecId anyway.
struct WireFundingCharge {
    std::string_view coin; // "BTC" — the perp's own name, not a pair
    std::string_view usdc; // SIGNED: negative was paid away
    std::string_view type; // "funding"
    std::uint64_t time_ms{0};
};

// Hyperliquid caps one userFunding answer at 500 rows and funding settles
// hourly, so this holds a full page and a little more.
inline constexpr std::size_t k_max_funding_charges = 512;

struct FundingChargeBuffer {
    std::array<WireFundingCharge, k_max_funding_charges> charges{};
};

// The body is a bare ARRAY, like every other account-scoped /info read.
[[nodiscard]] std::expected<std::span<const WireFundingCharge>, WireError>
parse_user_funding(std::string_view body, FundingChargeBuffer& storage);

} // namespace raikiri::venues::hyperliquid
