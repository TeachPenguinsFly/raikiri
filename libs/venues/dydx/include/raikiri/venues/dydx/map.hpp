#pragma once

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/dydx/wire.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

namespace raikiri::venues::dydx {

// Settled on the hour, every hour; no payload states it.
inline constexpr std::uint32_t k_funding_interval_s = 3600;

// Tickers are quoted "-USD" against the oracle, but margin and settlement are USDC.
inline constexpr std::string_view k_ticker_suffix = "-USD";
inline constexpr std::string_view k_margin_asset = "USDC";

enum class Refusal : std::uint8_t {
    Inactive,
    TooFine,   // a tick or lot below 1e-9, which FixedE9 cannot hold
    Disagrees, // tickSize/stepSize differ from the quantum fields
    Unreadable,
};

[[nodiscard]] std::string_view to_string(Refusal refusal) noexcept;

// The chain's own grid: subticksPerTick * 10^(quantumConversionExponent -
// atomicResolution - 6) and stepBaseQuantums * 10^atomicResolution, as e9.
[[nodiscard]] std::optional<FixedE9> quantum_tick(const WireMarket& wire) noexcept;
[[nodiscard]] std::optional<FixedE9> quantum_lot(const WireMarket& wire) noexcept;

// exact multiples of step without e9: some sizes exceed what FixedE9 holds as units
[[nodiscard]] std::optional<std::int64_t> exact_steps(std::string_view text, FixedE9 step) noexcept;

[[nodiscard]] std::expected<Instrument, Refusal> map_instrument(const WireMarket& wire);

[[nodiscard]] std::optional<TradeTick> map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init);

// The oracle is a validator median at its own precision and rarely on the tick:
// truncated toward zero onto it, never refused.
[[nodiscard]] std::optional<PriceTicks> oracle_ticks(std::string_view text, FixedE9 tick) noexcept;

[[nodiscard]] UnixNanos next_hour(UnixNanos now) noexcept;

// Nullopt when the row states no rate: absent is not zero.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireMarket& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireMarket& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<UnixNanos> parse_iso8601(std::string_view text) noexcept;

} // namespace raikiri::venues::dydx
