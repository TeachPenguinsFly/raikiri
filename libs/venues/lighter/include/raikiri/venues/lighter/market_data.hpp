#pragma once

// What the public stream says a market did: its prints, and the stats frame
// that carries mark, funding and the 24-hour window together.

#include <raikiri/venues/lighter/errors.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::lighter {

// One print off trade/<market_id>. `trades` and `liquidation_trades` are
// disjoint lists on the same frame — 50 and 50 with no id in common on the
// BTC backlog of 2026-09-24 — and both are prints on the tape.
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

[[nodiscard]] std::expected<std::span<const WirePrint>, WireError>
parse_trades(std::string_view frame, PrintBuffer& storage);

// One market's row off market_stats or spot_market_stats.
//
// THE TWO RATES ARE NOT A RATE AND A FORECAST. `current_funding_rate` is the
// rate accruing now and paid at the next hour; `funding_rate` is the one paid
// at `funding_timestamp`, the last hour mark. Both are PERCENT per hour —
// "0.0012" is 0.0012%, i.e. 1.2e-5 — which is why the mapper divides by a
// hundred rather than reading the string as a fraction.
//
// The daily volumes are JSON NUMBERS, not strings, and are kept as their raw
// text so no double is ever made of them.
struct WireMarketStats {
    std::uint32_t market_id{0};
    std::string_view mark_price;           // perps only
    std::string_view current_funding_rate; // perps only
    std::string_view last_funding_rate;    // perps only: `funding_rate`
    std::uint64_t funding_ms{0};           // perps only: when the last one was paid
    std::string_view base_volume;          // raw JSON number
    std::string_view quote_volume;         // raw JSON number
};

inline constexpr std::size_t k_max_stats = 1024;

struct StatsBuffer {
    std::array<WireMarketStats, k_max_stats> rows{};
};

struct WireStatsFrame {
    std::uint64_t ts_ms{0};
    std::span<const WireMarketStats> rows;
};

// Reads both channels and both shapes: `…/all` keys its rows by market id
// under one object, a single market's frame IS the row. An `…/all` update
// carries only the markets that changed — 84 of 235 in one measured second —
// so an absent market is unchanged, not delisted.
[[nodiscard]] std::expected<WireStatsFrame, WireError> parse_market_stats(std::string_view frame,
                                                                          StatsBuffer& storage);

} // namespace raikiri::venues::lighter
