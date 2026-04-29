#pragma once

#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::extended {

using WireError = wire::Error;

// One row of GET /api/v1/info/markets.
struct WireMarket {
    std::string_view name; // "BTC-USD": the wire name, and what every frame keys by
    std::string_view type; // "PERPETUAL" or "SPOT"
    // Filter on this, not on `active`, which stays true on delisted rows.
    std::string_view status;
    bool rfq{false};
    std::string_view tick;     // tradingConfig.minPriceChange
    std::string_view lot;      // tradingConfig.minOrderSizeChange
    std::string_view min_size; // tradingConfig.minOrderSize, in base units
    std::string_view funding_rate;
    std::string_view mark_price;
    // `nextFundingRate` on the wire, and a millisecond TIMESTAMP despite the name.
    std::uint64_t next_funding_ms{0};
    std::string_view volume_quote; // dailyVolume
    std::string_view volume_base;  // dailyVolumeBase
};

inline constexpr std::size_t k_max_markets = 1024;

struct MarketBuffer {
    std::array<WireMarket, k_max_markets> markets{};
};

[[nodiscard]] std::expected<std::span<const WireMarket>, WireError>
parse_markets(std::string_view body, MarketBuffer& storage);

} // namespace raikiri::venues::extended
