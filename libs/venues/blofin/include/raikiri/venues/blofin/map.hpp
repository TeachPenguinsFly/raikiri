#pragma once

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/blofin/catalogue.hpp>
#include <raikiri/venues/blofin/market_data.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace raikiri::venues::blofin {

[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// 0 for an unreadable unit or count, meaning unstated
[[nodiscard]] std::uint32_t interval_of(std::string_view count, std::string_view unit) noexcept;

[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market);

void apply_schedule(PerpInstrument& perp, const WireFundingRow& row) noexcept;

[[nodiscard]] std::optional<TradeTick> map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const InstrumentBase& base, UnixNanos ts_init);

// false when the row states no readable rate; a row without a period keeps the last one
[[nodiscard]] bool note_funding(FundingMemo& memo, const WireFundingRow& row) noexcept;

[[nodiscard]] bool note_mark(FundingMemo& memo, const WireMark& wire, const InstrumentBase& base) noexcept;

[[nodiscard]] FundingUpdate
merged(const InstrumentBase& base, const FundingMemo& memo, UnixNanos ts_event, UnixNanos ts_init) noexcept;

} // namespace raikiri::venues::blofin
