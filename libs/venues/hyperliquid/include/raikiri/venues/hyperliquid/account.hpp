#pragma once

#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::hyperliquid {

// `usdc` is signed by the venue; `type` is checked because the
// non-funding ledger shares this row shape
struct WireFundingCharge {
    std::string_view coin; // "BTC"
    std::string_view usdc; // signed; negative was paid away
    std::string_view type; // "funding"
    std::uint64_t time_ms{0};
};

// the venue caps one userFunding answer at 500 rows
inline constexpr std::size_t k_max_funding_charges = 512;

struct FundingChargeBuffer {
    std::array<WireFundingCharge, k_max_funding_charges> charges{};
};

[[nodiscard]] std::expected<std::span<const WireFundingCharge>, WireError>
parse_user_funding(std::string_view body, FundingChargeBuffer& storage);

// the account's tier rates; the referral discount is not yet taken off them
struct WireUserFees {
    std::string_view cross;      // "0.00045"
    std::string_view add;        // "0.00015"
    std::string_view spot_cross; // "0.0007"
    std::string_view spot_add;   // "0.0004"
    std::string_view referral;   // "0.04"
};

[[nodiscard]] std::expected<WireUserFees, WireError> parse_user_fees(std::string_view body);

// a spotClearinghouseState row; `hold` is what resting orders lock, and staked HYPE is not in it
struct WireSpotBalance {
    std::string_view coin;  // "USDC"
    std::string_view total; // "14.625485"
    std::string_view hold;  // "0.0"
};

inline constexpr std::size_t k_max_spot_balances = 256;

struct SpotBalanceBuffer {
    std::array<WireSpotBalance, k_max_spot_balances> balances{};
};

[[nodiscard]] std::expected<std::span<const WireSpotBalance>, WireError>
parse_spot_balances(std::string_view body, SpotBalanceBuffer& storage);

} // namespace raikiri::venues::hyperliquid
