#pragma once

#include <raikiri/book/ladder.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/paradex/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::paradex {

// an sbe mantissa over 1e8 as e9; nullopt for the null value or past the range
[[nodiscard]] std::optional<FixedE9> e8_to_e9(std::int64_t mantissa) noexcept;

[[nodiscard]] UnixNanos us_to_unix(std::int64_t us) noexcept;

[[nodiscard]] std::optional<Instrument> map_instrument(const WireMarket& wire, MarketType market);

[[nodiscard]] std::optional<book::Level> map_level(const WireLevel& wire, const InstrumentBase& base) noexcept;

[[nodiscard]] std::optional<TradeTick> map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init);

// funding accrues every second and is realised on any position change, so there is no next settlement;
// `interval_s` is the unit of the rate
[[nodiscard]] std::optional<FundingUpdate> map_funding(
    const WireFunding& wire, const InstrumentBase& base, std::int64_t mark, std::int64_t index, UnixNanos ts_init);

[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireSummary& wire, const InstrumentBase& base, UnixNanos ts_init);

} // namespace raikiri::venues::paradex
