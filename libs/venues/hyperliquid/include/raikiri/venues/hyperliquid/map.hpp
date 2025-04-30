#pragma once

// Wire types -> domain types.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/hyperliquid/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::hyperliquid {

// Hourly and uniform across every perp, hence a constant — and why `funding`
// must not be compared against a CEX's 8h rate unscaled.
inline constexpr std::uint32_t k_funding_interval_s = 3600;

// Venue ms -> UnixNanos; zero/absent yields ingest time, because a zero
// propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// 10^-decimals as e9 fixed point, or nullopt when the exponent has no
// representation. Integer throughout: deriving a step through a double would
// reintroduce exactly the rounding the fixed-point domain exists to avoid.
[[nodiscard]] std::optional<FixedE9> step_from_decimals(std::int64_t decimals) noexcept;

// Public market data ------------------------------------------------------

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt when the context quotes no funding at all: a row without funding must
// not be reported as one funding at zero. Unlike okx, mark_price IS populated —
// it rides the same payload.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireAssetCtx& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// nullopt for a delisted asset and for any row whose derived steps are
// unusable: every later price conversion divides by the tick size.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireAsset& wire, MarketType market);

} // namespace raikiri::venues::hyperliquid
