#pragma once

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/bitunix/wire.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace raikiri::venues::bitunix {

[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// a step stated as a count of decimal places
[[nodiscard]] std::optional<FixedE9> step_of(std::uint64_t places) noexcept;

// the spot depth endpoint aggregates at a decimal precision such as "0.01"
[[nodiscard]] std::string precision_text(FixedE9 tick);

// the live funding endpoints state percent; raikiri carries fractions
[[nodiscard]] std::optional<FixedE9> percent_to_fraction(std::string_view percent) noexcept;

// only rows the venue accepts an api order for are loaded
[[nodiscard]] std::optional<Instrument> map_pair(const WirePair& wire, MarketType market);

[[nodiscard]] std::optional<Bbo> map_bbo(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<BookSnapshot> map_book(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WirePrint& wire, std::uint64_t ts_ms, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireFunding& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const InstrumentBase& base, UnixNanos ts_init);

} // namespace raikiri::venues::bitunix
