#pragma once

// the account's commission rate and a symbol's leverage brackets; both are signed reads on usd-m futures

#include <raikiri/domain/terms.hpp>
#include <raikiri/venues/binance/errors.hpp>

#include <expected>
#include <string_view>
#include <vector>

namespace raikiri::venues::binance {

struct WireCommissionRate {
    double maker{0};
    double taker{0};
};

// /fapi/v1/commissionRate
[[nodiscard]] std::expected<WireCommissionRate, WireError> parse_commission_rate(std::string_view body);

// /fapi/v1/leverageBracket: `cum` is the maintenance amount; the answer is one object or an array of them
[[nodiscard]] std::expected<std::vector<MaintenanceTier>, WireError> parse_leverage_brackets(std::string_view body,
                                                                                             std::string_view symbol);

} // namespace raikiri::venues::binance
