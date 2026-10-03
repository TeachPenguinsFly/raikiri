#pragma once

#include <raikiri/book/ladder.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/lbank/wire.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace raikiri::venues::lbank {

[[nodiscard]] std::optional<Instrument> map_perp_instrument(const WirePerpInstrument& wire,
                                                            std::uint32_t funding_interval_s);

[[nodiscard]] std::optional<Instrument> map_spot_instrument(const WireSpotPair& wire);

// a volume can carry a float tail ("112441.5999999999"), so it is rounded to the lot
[[nodiscard]] std::optional<std::int64_t> lots_of(std::string_view text, FixedE9 lot) noexcept;

[[nodiscard]] std::optional<std::int64_t> ticks_of(std::string_view text, FixedE9 tick) noexcept;

// false when a row does not fit the instrument grid
[[nodiscard]] bool
apply_levels(const WireBook& wire, std::string_view symbol, const InstrumentBase& base, book::Ladders& ladders);

// false when the row does not fit the grid; a resting row clears the other side through its price
[[nodiscard]] bool apply_level(const WireLevel& level, const InstrumentBase& base, book::Ladders& ladders);

// one batch can reach a socket as several frames under one bNo, so only an older bNo is stale
[[nodiscard]] constexpr bool fresh_batch(std::uint64_t bno, std::uint64_t& newest) noexcept {
    if (bno < newest) {
        return false;
    }
    newest = bno;
    return true;
}

[[nodiscard]] bool seed_levels(const WireRestDepth& wire, const InstrumentBase& base, book::Ladders& ladders);

[[nodiscard]] bool same_touch(const book::Ladders& ladders, const WireRestDepth& wire, const InstrumentBase& base);

[[nodiscard]] std::optional<TradeTick> map_trade(const WireTrade& wire, const InstrumentBase& base, UnixNanos ts_init);

// settlements fall on multiples of the interval since the epoch
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireOverview& wire, const InstrumentBase& base, std::uint32_t interval_s, UnixNanos ts_init);

[[nodiscard]] std::optional<TickerStats>
map_perp_stats(const WireOverview& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<BookSnapshot>
map_spot_book(const WireSpotDepth& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<Bbo> top_of(const BookSnapshot& book) noexcept;

[[nodiscard]] std::optional<TradeTick>
map_spot_trade(const WireSpotTrade& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<TickerStats>
map_spot_stats(const WireSpotTick& wire, const InstrumentBase& base, UnixNanos ts_init);

} // namespace raikiri::venues::lbank
