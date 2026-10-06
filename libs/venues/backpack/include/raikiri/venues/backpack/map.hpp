#pragma once

#include <raikiri/domain/market_data.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/backpack/book.hpp>
#include <raikiri/venues/backpack/catalogue.hpp>
#include <raikiri/venues/backpack/market_data.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::backpack {

[[nodiscard]] UnixNanos us_to_unix(std::uint64_t us, UnixNanos fallback) noexcept;

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBookTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// for an instrument whose bookTicker has not sent a frame yet
[[nodiscard]] std::optional<Bbo>
map_touch(const WireDepthSnapshot& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WirePrint& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt when the frame states no rate, as on every spot pair
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireMarkPrice& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// the row has no timestamp, so ts_event stays zero
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos received);

struct OpenInterest {
    InstrumentId instrument{};
    FixedE9 base{};
};

[[nodiscard]] std::optional<OpenInterest> map_open_interest(const WireOpenInterest& wire,
                                                            const exec::InstrumentTable& table);

[[nodiscard]] std::optional<Instrument> map_instrument(const WireMarket& wire, MarketType market);

} // namespace raikiri::venues::backpack
