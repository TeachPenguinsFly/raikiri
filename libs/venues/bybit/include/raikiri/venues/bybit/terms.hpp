#pragma once

// a symbol's risk-limit tiers (public) and the account's fee rate for it (signed)

#include <raikiri/domain/terms.hpp>
#include <raikiri/venues/bybit/errors.hpp>

#include <expected>
#include <string_view>
#include <vector>

namespace raikiri::venues::bybit {

struct WireFeeRate {
    double maker{0};
    double taker{0};
};

// the `result` of /v5/account/fee-rate; the row named `symbol`, or the first where the venue names none
[[nodiscard]] std::expected<WireFeeRate, WireError> parse_fee_rate(std::string_view result, std::string_view symbol);

// the `result` of /v5/market/risk-limit: each tier's floor is the limit below it, and mmDeduction is its amount
[[nodiscard]] std::expected<std::vector<MaintenanceTier>, WireError> parse_risk_limit(std::string_view result,
                                                                                      std::string_view symbol);

} // namespace raikiri::venues::bybit
