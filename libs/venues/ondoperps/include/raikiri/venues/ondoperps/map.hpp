#pragma once

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/ondoperps/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::ondoperps {

inline constexpr std::uint32_t k_funding_interval_s = 3600;

// a disabled row is listed but not quoted, so it is not loaded
[[nodiscard]] std::optional<Instrument> map_instrument(const WireMarket& wire, MarketType market);

[[nodiscard]] std::optional<Bbo> map_top(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<BookSnapshot> map_book(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick> map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireFunding& wire, const InstrumentBase& base, PriceTicks mark, PriceTicks index, UnixNanos ts_init);

// the mark and the oracle truncated onto the tick
[[nodiscard]] std::optional<PriceTicks> mark_ticks(const WireMark& wire, const InstrumentBase& base) noexcept;
[[nodiscard]] std::optional<PriceTicks> oracle_ticks(const WireMark& wire, const InstrumentBase& base) noexcept;

struct Stats {
    FixedE9 volume_base{};
    FixedE6 volume_quote{};
    FixedE9 open_interest{};
};

[[nodiscard]] TickerStats map_ticker(const Stats& stats, const InstrumentBase& base, UnixNanos ts_init);

} // namespace raikiri::venues::ondoperps
