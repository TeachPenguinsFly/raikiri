#pragma once

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/ostium/wire.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace raikiri::venues::ostium {

// rollover accrues every arbitrum block; the rate is stated per hour
inline constexpr std::uint32_t k_rate_unit_s = 3600;
inline constexpr std::int64_t k_blocks_per_hour = 4 * 3600;

// the oracle has no tick and positions have no lot; both sit at the finest step raikiri holds
inline constexpr std::int64_t k_step_raw = 1;

[[nodiscard]] std::optional<Instrument> map_instrument(const WirePair& wire, MarketType market);

// an integer scaled by 1e18, truncated to 1e-9
[[nodiscard]] std::optional<FixedE9> e18_to_e9(std::string_view text) noexcept;

// the oracle quotes a price and no size, so both sizes are zero
[[nodiscard]] std::optional<Bbo> map_bbo(const WireQuote& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<PriceTicks> mid_ticks(const WireQuote& wire) noexcept;

// what a long pays per hour as a fraction of notional; a short pays premium minus carry, not its negation
[[nodiscard]] std::optional<FixedE9> long_rate_per_hour(const WireRollover& wire) noexcept;

[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireRollover& wire, const InstrumentBase& base, PriceTicks mid, UnixNanos ts_init);

struct FillPrint {
    TradeTick trade{};
    FixedE6 quote{};
};

// opens, closes, stops and liquidations; collateral edits are not trades
[[nodiscard]] std::optional<FillPrint> map_fill(const WireFill& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] TickerStats map_ticker(const WireRollover& wire,
                                     const InstrumentBase& base,
                                     FixedE9 volume_24h_base,
                                     FixedE6 volume_24h_quote,
                                     UnixNanos ts_init);

} // namespace raikiri::venues::ostium
