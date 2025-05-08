#pragma once

// Wire types -> domain types.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/htx/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::htx {

// Venue milliseconds -> UnixNanos. A zero or absent venue timestamp yields
// local ingest time: a zero propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// Exact decimal-or-exponent token -> 1e9 fixed point. Refuses significance
// finer than 1e-9 rather than rounding it away: a tick flattened to zero makes
// every later price conversion a division by zero.
[[nodiscard]] std::optional<FixedE9> exact_e9(std::string_view token);

// The same reader, truncating below 1e-9 instead of refusing. Rates only: HTX
// pads every funding rate to eighteen fractional digits with a tail that is
// not always zero, so an exact read would present a running venue as silence.
// What remains is eight significant figures of a rate no order divides by.
[[nodiscard]] std::optional<FixedE9> rate_e9(std::string_view token);

// Public market data ------------------------------------------------------

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Returns nullopt when the frame quotes no rate at all — a contract without
// funding must not be reported as one funding at zero. mark_price stays
// undefined because this frame carries none. The interval comes from the
// instrument, which is where the catalogue put it.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireFunding& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// step0 is a whole book every frame, so no state is carried between them and
// there is no chain to break: the newest frame simply replaces what shm holds.
// venue_seq takes tick.mrid, the only field on this payload that orders frames.
[[nodiscard]] std::optional<BookSnapshot>
map_depth(const WireDepth& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Builds a domain instrument from a /swap_contract_info row. Returns nullopt
// for anything not listed, anything that is not a USDT-M swap, and any row
// whose tick or contract size failed to parse.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market);

} // namespace raikiri::venues::htx
