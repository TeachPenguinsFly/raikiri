#pragma once

#include <raikiri/book/ladder.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/phemex/book.hpp>
#include <raikiri/venues/phemex/catalogue.hpp>
#include <raikiri/venues/phemex/market_data.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace raikiri::venues::phemex {

// each instrument uses one format: decimal strings or scaled integers
enum class Encoding : std::uint8_t { Real, Scaled };

// decoding data the Instrument does not carry
struct Listing {
    Encoding encoding{Encoding::Real};
    std::int64_t price_step{0}; // Scaled: the tick in the frame's units
    std::int64_t qty_step{0};   // Scaled: the lot; whole contracts on an inverse perp
    std::uint32_t price_scale{0};
    std::uint32_t ratio_scale{0};
    std::uint32_t volume_scale{0};   // spot: the base coin's valueScale
    std::uint32_t turnover_scale{0}; // spot: the quote coin's; inverse: the settle coin's
    std::uint32_t funding_interval_s{0};
};

struct Listed {
    Instrument instrument;
    Listing listing;
};

// Rp/Rq: a decimal string, exactly on the step or refused.
[[nodiscard]] std::optional<std::int64_t> real_steps(std::string_view text, FixedE9 step) noexcept;

// Ep/Ev: an already-scaled integer, exactly on the scaled step or refused.
[[nodiscard]] std::optional<std::int64_t> scaled_steps(std::int64_t value, std::int64_t step) noexcept;

// Er/Ev scaled by 10^scale, as e9.
[[nodiscard]] std::optional<FixedE9> scaled_e9(std::int64_t value, std::uint32_t scale) noexcept;

[[nodiscard]] UnixNanos ns_to_unix(std::uint64_t ns, UnixNanos fallback) noexcept;

// no next funding time is published; settlements sit on the utc grid of the interval
[[nodiscard]] UnixNanos next_funding_time(UnixNanos now, std::uint32_t interval_s) noexcept;

// nullopt unless Listed with a grid that maps
[[nodiscard]] std::optional<Listed> map_linear_perp(const WirePerp& wire);
[[nodiscard]] std::optional<Listed> map_inverse_perp(const WireProduct& wire, std::uint32_t settle_scale);
[[nodiscard]] std::optional<Listed>
map_spot(const WireProduct& wire, std::uint32_t base_scale, std::uint32_t quote_scale);

// refused counts Listed rows that would not map
struct MappedCatalogue {
    std::vector<Listed> listed;
    std::size_t refused{0};
};

[[nodiscard]] MappedCatalogue map_catalogue(const WireCatalogue& wire, MarketType market);

// nullopt when any level is off the grid
[[nodiscard]] std::optional<std::size_t>
to_levels(std::span<const WireRealLevel> wire, const InstrumentBase& base, std::span<book::Level> out);
[[nodiscard]] std::optional<std::size_t>
to_levels(std::span<const WireScaledLevel> wire, const Listing& listing, std::span<book::Level> out);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireRealPrint& wire, const InstrumentBase& base, UnixNanos ts_init);
[[nodiscard]] std::optional<TradeTick>
map_trade(const WireScaledPrint& wire, const InstrumentBase& base, const Listing& listing, UnixNanos ts_init);

[[nodiscard]] std::optional<FundingUpdate> map_funding(const WirePackRow& wire,
                                                       const InstrumentBase& base,
                                                       const Listing& listing,
                                                       std::uint64_t ts_ns,
                                                       UnixNanos ts_init);
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireInverseTicker& wire, const InstrumentBase& base, const Listing& listing, UnixNanos ts_init);

[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WirePackRow& wire, const InstrumentBase& base, std::uint64_t ts_ns, UnixNanos ts_init);
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireInverseTicker& wire, const InstrumentBase& base, const Listing& listing, UnixNanos ts_init);
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireSpotTicker& wire, const InstrumentBase& base, const Listing& listing, UnixNanos ts_init);

} // namespace raikiri::venues::phemex
