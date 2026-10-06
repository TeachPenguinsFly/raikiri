#pragma once

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/upbit/wire.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace raikiri::venues::upbit {

// the tick moves with the price tier; this is the finest step any tier prints
inline constexpr FixedE9 k_price_step = FixedE9::from_raw(10);

// sizes print to eight places, but a market quoted in fractions of a won holds levels past
// what eight places fit in an int64; there the lot widens by decades and the rest is dropped
[[nodiscard]] FixedE9 lot_for(std::string_view quote, std::string_view price) noexcept;

// a market listed as "KRW-BTC"; nullopt when the code is not quote-base
[[nodiscard]] std::optional<Instrument>
map_instrument(const WireMarket& wire, std::string_view price, VenueId venue, UnixNanos now);

// stamps arrive in milliseconds or microseconds; zero stays unstamped
[[nodiscard]] UnixNanos stamp(std::uint64_t ts) noexcept;

// both sides or nothing; a crossed or unordered top is refused
[[nodiscard]] std::optional<Bbo> map_bbo(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init);

// a market with no orders maps to an empty book, not a refusal
[[nodiscard]] std::optional<BookSnapshot> map_book(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick> map_trade(const WireTrade& wire, const InstrumentBase& base, UnixNanos ts_init);

// `stamped` false leaves ts_event zero
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const InstrumentBase& base, bool stamped, UnixNanos ts_init);

} // namespace raikiri::venues::upbit
