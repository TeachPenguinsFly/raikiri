#pragma once

#include <raikiri/venues/htx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::htx {

// direction is already the aggressor's side; amount is contracts, quantity base units
struct WireTrade {
    std::string_view contract_code;
    std::string_view price;
    std::string_view amount;
    std::string_view quantity;
    bool aggressor_bought{false};
    std::uint64_t trade_id{0};
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

[[nodiscard]] std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage);

// amount is base, trade_turnover quote; vol is contracts and is not read
struct WireDetail {
    std::string_view contract_code;
    std::string_view amount;         // base units
    std::string_view trade_turnover; // quote units
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireDetail, WireError> parse_detail(std::string_view frame);

// settlement_time is the next settlement; funding_time is message build time here and means something else on rest
struct WireFunding {
    std::string_view contract_code;
    std::string_view funding_rate;
    std::string_view estimated_rate; // JSON null in every frame observed
    std::uint64_t settlement_ms{0};
    std::uint64_t funding_ms{0};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireFunding, WireError> parse_funding(std::string_view frame);

} // namespace raikiri::venues::htx
