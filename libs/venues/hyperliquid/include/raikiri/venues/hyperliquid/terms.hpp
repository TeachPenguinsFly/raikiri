#pragma once

// a coin's margin tiers, from the `meta` answer the catalogue also reads

#include <raikiri/domain/terms.hpp>
#include <raikiri/venues/hyperliquid/errors.hpp>

#include <expected>
#include <string_view>
#include <vector>

namespace raikiri::venues::hyperliquid {

// the coin's marginTableId picks a table; an id under 50 is no table but one tier at that leverage. a tier's
// maintenance rate is half its initial one, 1 / (2 * maxLeverage), and its amount keeps the margin continuous
[[nodiscard]] std::expected<std::vector<MaintenanceTier>, WireError> parse_margin_tiers(std::string_view meta,
                                                                                        std::string_view coin);

} // namespace raikiri::venues::hyperliquid
