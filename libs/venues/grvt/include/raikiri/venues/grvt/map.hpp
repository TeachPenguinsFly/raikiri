#pragma once

// Wire types -> domain types.

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/grvt/wire.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace raikiri::venues::grvt {

// A zero venue time yields ingest time: a zero in a latency histogram
// poisons it.
[[nodiscard]] constexpr UnixNanos ns_or(std::uint64_t ns, UnixNanos fallback) noexcept {
    return ns == 0 ? fallback : UnixNanos{ns};
}

// Exact against the instrument's own step, or nullopt: a value off the grid
// means the venue changed a step, and rounding it is how a price lands one
// tick away from where it was.
[[nodiscard]] std::optional<PriceTicks> to_ticks(std::string_view text, FixedE9 tick) noexcept;
[[nodiscard]] std::optional<QtyLots> to_lots(std::string_view text, FixedE9 lot) noexcept;

// GRVT states every funding figure in PERCENT; this system carries ratios.
// UNDEF for an absent or unreadable figure, never zero.
[[nodiscard]] FixedE9 percent_to_ratio(std::string_view text) noexcept;

// Nullopt for anything but an active perpetual, for any market but Perp, and
// for a row whose steps will not parse.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire,
                                                       MarketType market);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_event, UnixNanos ts_init);

// Nullopt when the frame states no rate: a contract quoting none must not
// read as one funding at zero. The interval is the frame's own when it states
// one, the catalogue's otherwise.
[[nodiscard]] std::optional<FundingUpdate> map_funding(const WireTicker& wire,
                                                       const Instrument& instrument,
                                                       UnixNanos ts_event,
                                                       UnixNanos ts_init);

[[nodiscard]] std::optional<TickerStats> map_ticker_stats(const WireTicker& wire,
                                                          const InstrumentBase& base,
                                                          UnixNanos ts_event,
                                                          UnixNanos ts_init);

} // namespace raikiri::venues::grvt
