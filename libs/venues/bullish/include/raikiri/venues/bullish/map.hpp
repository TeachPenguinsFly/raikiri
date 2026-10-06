#pragma once

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/bullish/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::bullish {

// settled at the end of every utc hour
inline constexpr std::uint32_t k_funding_interval_s = 3600;

[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms) noexcept;

// amm fills print at the price precision, finer than the order tick; that finer grid is the instrument's
[[nodiscard]] std::optional<Instrument> map_instrument(const WireMarket& wire, MarketType market);

[[nodiscard]] std::optional<BookSnapshot> map_book(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<Bbo> top_of(const BookSnapshot& book) noexcept;

[[nodiscard]] std::optional<TradeTick> map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init);

// the asset's index truncated onto the market's tick
[[nodiscard]] std::optional<PriceTicks> index_ticks(const WireIndex& wire, const InstrumentBase& base) noexcept;

[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireTick& wire, const InstrumentBase& base, PriceTicks index, UnixNanos ts_init);

[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTick& wire, const InstrumentBase& base, UnixNanos ts_init);

// the ranges of consecutive snapshots meet; a jump is counted, and the next snapshot heals it
struct SeqRange {
    std::uint64_t last{0};
    std::uint64_t gaps{0};

    [[nodiscard]] bool admit(const WireBook& wire) noexcept {
        if (wire.seq_last <= last) {
            return false;
        }
        if (last != 0 && wire.seq_first != last + 1) {
            ++gaps;
        }
        last = wire.seq_last;
        return true;
    }
};

} // namespace raikiri::venues::bullish
