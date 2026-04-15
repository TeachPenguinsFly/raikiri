#pragma once

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/grvt/wire.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace raikiri::venues::grvt {

[[nodiscard]] constexpr UnixNanos ns_or(std::uint64_t ns, UnixNanos fallback) noexcept {
    return ns == 0 ? fallback : UnixNanos{ns};
}

// Nullopt off the instrument's grid; never rounded.
[[nodiscard]] std::optional<PriceTicks> to_ticks(std::string_view text, FixedE9 tick) noexcept;
[[nodiscard]] std::optional<QtyLots> to_lots(std::string_view text, FixedE9 lot) noexcept;

[[nodiscard]] FixedE9 percent_to_ratio(std::string_view text) noexcept;

[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_event, UnixNanos ts_init);

// Nullopt when the frame states no rate, never a zero rate.
[[nodiscard]] std::optional<FundingUpdate> map_funding(const WireTicker& wire,
                                                       const Instrument& instrument,
                                                       UnixNanos ts_event,
                                                       UnixNanos ts_init);

[[nodiscard]] std::optional<TickerStats> map_ticker_stats(const WireTicker& wire,
                                                          const InstrumentBase& base,
                                                          UnixNanos ts_event,
                                                          UnixNanos ts_init);

} // namespace raikiri::venues::grvt
