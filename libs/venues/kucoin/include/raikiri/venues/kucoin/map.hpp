#pragma once

// Wire types -> domain types.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/kucoin/wire.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::kucoin {

// Venue epochs -> UnixNanos. A zero or absent venue timestamp yields local
// ingest time: a zero propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;
[[nodiscard]] UnixNanos ns_to_unix(std::uint64_t ns, UnixNanos fallback) noexcept;

// parse_decimal_e9 plus an optional "E±n" suffix. Exposed because the
// catalogue's tick sizes reach 1.0E-10, one decade below what FixedE9 models,
// and the refusal of those rows is a property worth pinning by test.
[[nodiscard]] std::optional<FixedE9> parse_number_e9(std::string_view text) noexcept;

// Public market data ------------------------------------------------------

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBbo& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// `symbol` overrides the DTO's own, because the REST form names the funding
// index rather than the contract. Returns nullopt when the frame quotes no
// rate at all — a contract without funding must not be reported as one
// funding at zero. mark_price is left undefined because KuCoin ships mark
// under a different subject; a zero there would read as a real price of zero.
[[nodiscard]] std::optional<FundingUpdate> map_funding(const WireFundingRate& wire,
                                                       std::string_view symbol,
                                                       const exec::InstrumentTable& table,
                                                       UnixNanos ts_init);

// Private order flow ------------------------------------------------------

// One frame kind carries the whole lifecycle, so this is where an ack, a fill
// and a cancel part company. A `filled` notice yields nullopt: the trades that
// filled the order already arrived as `match`, and emitting both counts the
// last fill twice.
[[nodiscard]] std::optional<OrderEvent> map_order_change(const WireOrderChange& wire,
                                                         const exec::InstrumentTable& table,
                                                         UnixNanos ts_init);

// Reconciliation pulls ----------------------------------------------------

[[nodiscard]] std::optional<exec::OrderStatusReport>
map_open_order(const WireOrder& wire, const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::FillReport> map_fill(const WireFill& wire,
                                                       const exec::InstrumentTable& table);

// The venue's avg_entry is kept when it happens to land on a tick and left
// undefined otherwise: an average of fills has no reason to be a multiple of
// the price step, and the quantity is the part reconciliation acts on.
[[nodiscard]] std::optional<exec::PositionReport>
map_position(const WirePosition& wire, const exec::InstrumentTable& table);

// Catalogue ---------------------------------------------------------------

// Returns nullopt for anything not Open, anything that is not a perpetual,
// anything inverse, and any row whose steps failed to parse.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market);

// The spot catalogue lives on the other host and states its steps as
// increments rather than as tick/lot sizes.
[[nodiscard]] std::optional<Instrument> map_spot_symbol(const WireSpotSymbol& wire);

} // namespace raikiri::venues::kucoin
