#pragma once

#include <raikiri/domain/market_data.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/cryptocom/catalogue.hpp>
#include <raikiri/venues/cryptocom/market_data.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::cryptocom {

// the venue states no period; perpetuals settle on the hour
inline constexpr std::uint32_t k_funding_interval_s = 3600;

// no frame carries the settlement time: the next top of the hour
[[nodiscard]] std::uint64_t next_settlement_ms(std::uint64_t ms) noexcept;

[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

[[nodiscard]] std::optional<TradeTick> map_trade(const WireTrade& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const InstrumentBase& base, UnixNanos ts_init);

// each publishes the merged memo, and none publishes before a rate is known
[[nodiscard]] std::optional<FundingUpdate>
map_funding_rate(const WireValue& wire, const InstrumentBase& base, FundingMemo& memo, UnixNanos ts_init);

[[nodiscard]] std::optional<FundingUpdate>
map_estimated_rate(const WireValue& wire, const InstrumentBase& base, FundingMemo& memo, UnixNanos ts_init);

[[nodiscard]] std::optional<FundingUpdate>
map_mark(const WireValue& wire, const InstrumentBase& base, FundingMemo& memo, UnixNanos ts_init);

// an index is off the perpetual's grid, so it is truncated to it
[[nodiscard]] std::optional<FundingUpdate>
map_index(const WireValue& wire, const InstrumentBase& base, FundingMemo& memo, UnixNanos ts_init);

[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market);

} // namespace raikiri::venues::cryptocom
