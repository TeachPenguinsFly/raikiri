#pragma once

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/pacifica/wire.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace raikiri::venues::pacifica {

// Settled at the top of every UTC hour.
inline constexpr std::uint32_t k_funding_interval_s = 3600;

inline constexpr std::string_view k_perp_quote = "USDC";

[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

[[nodiscard]] UnixNanos next_settlement(UnixNanos at) noexcept;

[[nodiscard]] std::optional<Instrument> map_instrument(const WireMarket& wire, MarketType market);

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBbo& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<WireBbo> top_of(const WireBook& wire) noexcept;

[[nodiscard]] std::optional<BookSnapshot>
map_book(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WirePrice& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WirePrice& wire, const InstrumentBase& base, UnixNanos ts_init);

// `li` rises across every symbol, so a gap is normal; a repeat or a step back
// is a frame older than one already published.
struct LastId {
    std::uint64_t last{0};

    [[nodiscard]] bool admit(std::uint64_t li) noexcept {
        if (li <= last) {
            return false;
        }
        last = li;
        return true;
    }
};

} // namespace raikiri::venues::pacifica
