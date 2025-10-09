#pragma once

// What the venue says this account is long or short — twice, because the
// stream and the REST read state it in different shapes.

#include <raikiri/venues/binance/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::binance {

// One position line out of ACCOUNT_UPDATE's a.P array.
struct WirePosition {
    std::string_view symbol;        // s
    std::string_view position_amt;  // pa: signed
    std::string_view entry_price;   // ep
    std::string_view position_side; // ps: BOTH, LONG, SHORT
};

inline constexpr std::size_t k_max_positions = 512;

struct PositionBuffer {
    std::array<WirePosition, k_max_positions> positions{};
};

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_account_update(std::string_view data, PositionBuffer& storage);

// One row of GET /fapi/v2/positionRisk, which is what reconciliation reads:
// the same holding under the endpoint's own field names.
struct WirePositionRisk {
    std::string_view symbol;
    std::string_view position_amt;
    std::string_view entry_price;
};

[[nodiscard]] std::expected<std::span<const WirePositionRisk>, WireError>
parse_position_risk(std::string_view body, std::span<WirePositionRisk> storage);

} // namespace raikiri::venues::binance
