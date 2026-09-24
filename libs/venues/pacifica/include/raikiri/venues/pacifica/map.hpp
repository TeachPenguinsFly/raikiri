#pragma once

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/pacifica/wire.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace raikiri::venues::pacifica {

// settled at the top of every utc hour
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

// after the hour `funding` lags while `next_funding` holds the accruing rate;
// use next_funding until the roll, for at most this long
inline constexpr std::uint64_t k_roll_hold_ns = 90'000'000'000ULL;

struct FundingRoll {
    std::uint64_t hour{0};
    std::uint64_t hold_until_ns{0};
    FixedE9 held{};
    FixedE9 funding{};
    FixedE9 next{};

    void apply(FundingUpdate& update) noexcept;
};

// `li` rises across all symbols, so gaps are normal; a repeat or step back is stale
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
