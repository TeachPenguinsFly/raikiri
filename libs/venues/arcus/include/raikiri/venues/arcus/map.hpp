#pragma once

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/arcus/wire.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace raikiri::venues::arcus {

// settled at the top of every utc hour
inline constexpr std::uint32_t k_funding_interval_s = 3600;

[[nodiscard]] UnixNanos us_to_unix(std::uint64_t us) noexcept;

[[nodiscard]] AssetClass asset_class_of(std::string_view category) noexcept;

[[nodiscard]] std::optional<Instrument> map_instrument(const WireMarket& wire, MarketType market);

[[nodiscard]] std::optional<Bbo> map_bbo(const WireBbo& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick> map_trade(const WirePrint& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireStats& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireStats& wire, const InstrumentBase& base, UnixNanos ts_init);

} // namespace raikiri::venues::arcus
