#pragma once

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/options/convention.hpp>
#include <raikiri/venues/derive/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::derive {

// iv arrives as a fraction; vega per vol point and theta per day match black-76 on a live frame
inline constexpr options::Convention k_convention{};

// `f` is a rate per hour that accrues continuously
inline constexpr std::uint32_t k_funding_basis_s = 3600;

// listed rows that are inactive or not yet activated at `now` are refused
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market, UnixNanos now);

// zero falls back to ingest time
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// both sides or nothing
[[nodiscard]] std::optional<Bbo> map_bbo(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<BookSnapshot> map_book(const WireBook& wire, const InstrumentBase& base, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick> map_trade(const WireTrade& wire, const InstrumentBase& base, UnixNanos ts_init);

// nullopt off perpetuals; a perpetual can quote exactly 0
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const InstrumentBase& base, UnixNanos ts_init);

// nullopt off options
[[nodiscard]] std::optional<OptionGreeks>
map_greeks(const WireTicker& wire, const InstrumentBase& base, UnixNanos ts_init);

// the quote column is the premium traded on options and the notional on perpetuals
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const InstrumentBase& base, MarketType market, UnixNanos ts_init);

} // namespace raikiri::venues::derive
