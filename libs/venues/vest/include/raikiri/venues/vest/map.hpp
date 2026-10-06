#pragma once

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/vest/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::vest {

// settled at the top of every utc hour
inline constexpr std::uint32_t k_funding_interval_s = 3600;

[[nodiscard]] UnixNanos next_settlement(UnixNanos at) noexcept;

// halted rows are listed but not quoted, so they are not loaded
[[nodiscard]] std::optional<Instrument> map_instrument(const WireMarket& wire, MarketType market);

[[nodiscard]] std::optional<BookSnapshot> map_book(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<Bbo> top_of(const BookSnapshot& book) noexcept;

[[nodiscard]] std::optional<TradeTick> map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const InstrumentBase& base, UnixNanos ts_init);

struct Stats {
    FixedE9 volume_base{};
    FixedE6 volume_quote{};
    FixedE9 open_interest{};
};

[[nodiscard]] TickerStats map_ticker(const Stats& stats, const InstrumentBase& base, UnixNanos ts_init);

} // namespace raikiri::venues::vest
