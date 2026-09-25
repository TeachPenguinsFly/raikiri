#pragma once

#include <raikiri/venues/lighter/errors.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::lighter {

// `trades` and `liquidation_trades` are disjoint lists, and both are prints.
struct WirePrint {
    std::uint32_t market_id{0};
    std::uint64_t trade_id{0};
    std::string_view price;
    std::string_view size;
    std::uint64_t ts_ms{0};
    bool maker_is_ask{false}; // is_maker_ask: true => the aggressor bought
};

inline constexpr std::size_t k_max_prints = 256;

struct PrintBuffer {
    std::array<WirePrint, k_max_prints> prints{};
};

[[nodiscard]] std::expected<std::span<const WirePrint>, WireError> parse_trades(std::string_view frame,
                                                                                PrintBuffer& storage);

// current_funding_rate accrues now; funding_rate was paid at funding_timestamp,
// the last hour mark. Both are percent per hour.
struct WireMarketStats {
    std::uint32_t market_id{0};
    std::string_view mark_price;
    std::string_view current_funding_rate;
    std::string_view last_funding_rate; // wire name `funding_rate`
    std::uint64_t funding_ms{0};        // when the last one was paid
    std::string_view base_volume;       // raw JSON number
    std::string_view quote_volume;      // raw JSON number
};

inline constexpr std::size_t k_max_stats = 1024;

struct StatsBuffer {
    std::array<WireMarketStats, k_max_stats> rows{};
};

struct WireStatsFrame {
    std::uint64_t ts_ms{0};
    std::span<const WireMarketStats> rows;
};

// An `…/all` update carries only the markets that changed; an absent one is not delisted.
[[nodiscard]] std::expected<WireStatsFrame, WireError> parse_market_stats(std::string_view frame, StatsBuffer& storage);

} // namespace raikiri::venues::lighter
