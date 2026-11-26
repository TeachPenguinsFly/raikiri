#pragma once

// an instrument family's position tiers (public) and the account's fee rates for it (signed)

#include <raikiri/domain/terms.hpp>
#include <raikiri/venues/okx/errors.hpp>

#include <expected>
#include <optional>
#include <string_view>
#include <vector>

namespace raikiri::venues::okx {

// okx states a charge as a negative rate and a rebate as a positive one; these are flipped to charge-positive.
// a pair is absent where the venue left it empty
struct WireTradeFee {
    struct Pair {
        double maker{0};
        double taker{0};
    };

    std::optional<Pair> coin; // `maker`/`taker`: spot and coin-margined contracts
    std::optional<Pair> usdt; // `makerU`/`takerU`
    std::optional<Pair> usdc; // `makerUSDC`/`takerUSDC`
};

// the first row of /api/v5/account/trade-fee
[[nodiscard]] std::expected<WireTradeFee, WireError> parse_trade_fee(std::string_view body);

// /api/v5/public/position-tiers. floor and cap are minSz and maxSz in contracts, and the amount is zero: okx
// charges a tier's mmr on the whole position
[[nodiscard]] std::expected<std::vector<MaintenanceTier>, WireError> parse_position_tiers(std::string_view body);

// "BTC-USDT-SWAP" and "BTC-USDT-251226" are both of family "BTC-USDT"; empty for anything shorter
[[nodiscard]] std::string_view inst_family_of(std::string_view inst_id) noexcept;

} // namespace raikiri::venues::okx
