#pragma once

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/lighter/catalogue.hpp>
#include <raikiri/venues/lighter/market_data.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::lighter {

// Hourly on the hour; no row or frame states the period.
inline constexpr std::uint32_t k_funding_interval_s = 3600;

// Every perp is margined and quoted in USDC; the row names only the base.
inline constexpr std::string_view k_perp_quote = "USDC";

[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// Nullopt past nine places, which e9 cannot hold.
[[nodiscard]] std::optional<FixedE9> step_of(std::uint32_t decimals) noexcept;

[[nodiscard]] std::optional<Instrument> map_instrument(const WireMarket& wire, MarketType market);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init);

// Nullopt when no rate is quoted: a missing rate is not a rate of zero.
[[nodiscard]] std::optional<FundingUpdate> map_funding(const WireMarketStats& wire,
                                                       const InstrumentBase& base,
                                                       std::uint64_t frame_ms,
                                                       UnixNanos ts_init);

[[nodiscard]] std::optional<TickerStats> map_ticker_stats(const WireMarketStats& wire,
                                                          const InstrumentBase& base,
                                                          std::uint64_t frame_ms,
                                                          UnixNanos ts_init);

} // namespace raikiri::venues::lighter
