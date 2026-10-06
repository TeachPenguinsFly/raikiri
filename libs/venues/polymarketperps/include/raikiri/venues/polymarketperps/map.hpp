#pragma once

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/polymarketperps/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::polymarketperps {

inline constexpr std::uint32_t k_funding_interval_s = 3600;

[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market);

[[nodiscard]] std::optional<Bbo> map_bbo(const WireBbo& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<BookSnapshot> map_book(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<Bbo> top_of(const BookSnapshot& book) noexcept;

// settlement prints are left out
[[nodiscard]] std::optional<TradeTick> map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const InstrumentBase& base, UnixNanos ts_init);

struct Stats {
    FixedE6 volume_quote{};
    FixedE9 open_interest{};
};

[[nodiscard]] TickerStats map_ticker(const Stats& stats, const InstrumentBase& base, UnixNanos ts_init);

// `sq` rises across all channels, so gaps are normal; a repeat or step back is stale
struct LastSeq {
    std::uint64_t last{0};

    [[nodiscard]] bool admit(std::uint64_t sq) noexcept {
        if (sq <= last) {
            return false;
        }
        last = sq;
        return true;
    }
};

} // namespace raikiri::venues::polymarketperps
