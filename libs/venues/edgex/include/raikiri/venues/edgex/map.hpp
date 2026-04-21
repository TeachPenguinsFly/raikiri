#pragma once

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/edgex/wire.hpp>

#include <optional>
#include <string_view>

namespace raikiri::venues::edgex {

[[nodiscard]] std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market);

// The mark carries up to eighteen decimals and never sits on the tick grid, so
// it is truncated onto the grid rather than refused.
[[nodiscard]] PriceTicks mark_ticks(std::string_view text, FixedE9 tick_size) noexcept;

[[nodiscard]] std::optional<TradeTick>
map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const Instrument& instrument, UnixNanos ts_init);

[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const InstrumentBase& base, UnixNanos ts_init);

} // namespace raikiri::venues::edgex
