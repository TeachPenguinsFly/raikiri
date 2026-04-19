#pragma once

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/apex/wire.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace raikiri::venues::apex {

// No row or frame states the period; the funding history pays on every UTC hour.
inline constexpr std::uint32_t k_funding_interval_s = 3600;

[[nodiscard]] UnixNanos us_to_unix(std::uint64_t us, UnixNanos fallback) noexcept;

// "2026-09-25T06:00:00Z", whole seconds in UTC and nothing else.
[[nodiscard]] std::optional<UnixNanos> parse_utc(std::string_view text) noexcept;

// Nullopt for a contract not enabled for trading: a disabled one still
// subscribes and streams a stale price.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const InstrumentBase& base, UnixNanos ts_init);

// Ticker deltas are partial, so each field is merged into what was said before.
struct FundingMemo {
    FixedE9 rate{};
    PriceTicks mark{};
    UnixNanos next_funding{};
};

[[nodiscard]] std::optional<FundingUpdate> map_funding(const WireTicker& wire,
                                                       const PerpInstrument& perp,
                                                       FundingMemo& memo,
                                                       UnixNanos ts_init);

[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const InstrumentBase& base, UnixNanos ts_init);

} // namespace raikiri::venues::apex
