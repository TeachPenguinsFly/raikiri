#pragma once

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/bitstamp/wire.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace raikiri::venues::bitstamp {

// the funding history steps eight hours: 00:00, 08:00 and 16:00 utc
inline constexpr std::uint32_t k_funding_interval_s = 28'800;

// enabled rows of `market` only; a fiat pair is fx, a perpetual takes the class the venue states
[[nodiscard]] std::optional<Instrument> map_instrument(const WireMarket& wire, MarketType market, UnixNanos now);

// the market the channel names: order_book_btcusd is btcusd
[[nodiscard]] std::string_view symbol_of(std::string_view channel) noexcept;

// both sides or nothing; a crossed or unordered book is refused
[[nodiscard]] std::optional<BookSnapshot> map_book(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick> map_trade(const WireTrade& wire, const InstrumentBase& base, UnixNanos ts_init);

// mark and index are cut to the tick
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireFunding& wire, const InstrumentBase& base, UnixNanos ts_init);

// quote turnover is the base volume at the 24-hour vwap
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const InstrumentBase& base, UnixNanos ts_init);

} // namespace raikiri::venues::bitstamp
