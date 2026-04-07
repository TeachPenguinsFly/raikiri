#pragma once

// Wire types -> domain types.
//
// Every mapper here takes the instrument already resolved: frames name a
// market by integer id, and the id -> instrument lookup is the caller's,
// held beside the catalogue it was built from.

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/lighter/catalogue.hpp>
#include <raikiri/venues/lighter/market_data.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::lighter {

// Every perpetual settles funding hourly, on the hour. Stated on
// docs.lighter.xyz/trading/funding ("Funding payments occur at each hour
// mark") and measured rather than taken from it: /api/v1/fundings for BTC
// (1), ETH (0) and IBM (188) over the 48 hours to 2026-09-24 20:00Z returned
// 48 settlements each, 47 gaps of exactly 3600 s. No row or frame states the
// period, so the period is this constant.
inline constexpr std::uint32_t k_funding_interval_s = 3600;

// Every perp is margined and quoted in USDC; the row names only the base.
inline constexpr std::string_view k_perp_quote = "USDC";

[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// "0.1" from price_decimals 1, exactly; nullopt past nine places, which e9
// cannot hold.
[[nodiscard]] std::optional<FixedE9> step_of(std::uint32_t decimals) noexcept;

// Nullopt for an inactive row, a row of another market type, or one whose
// decimals or pair will not map.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireMarket& wire, MarketType market);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init);

// Lighter's OWN rate for the hour now accruing, read off its own stats frame.
// Nullopt on a row that quotes no rate — a spot row, or a perp frame that
// omitted it — because a missing rate is not a rate of zero.
[[nodiscard]] std::optional<FundingUpdate> map_funding(const WireMarketStats& wire,
                                                       const InstrumentBase& base,
                                                       std::uint64_t frame_ms,
                                                       UnixNanos ts_init);

[[nodiscard]] std::optional<TickerStats> map_ticker_stats(const WireMarketStats& wire,
                                                          const InstrumentBase& base,
                                                          std::uint64_t frame_ms,
                                                          UnixNanos ts_init);

} // namespace raikiri::venues::lighter
