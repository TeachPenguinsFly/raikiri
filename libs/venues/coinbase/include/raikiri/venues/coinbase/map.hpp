#pragma once

// Wire types -> domain types.
//
// Where Coinbase's vocabulary becomes ours: strings become instrument-scaled
// integers, product ids become interned ids. Everything above this file is
// venue-agnostic; everything below it is Coinbase-shaped.
//
// Three differences from the okx adapter are structural rather than cosmetic.
//
// This is the SPOT path. Coinbase Exchange lists no perpetuals and no dated
// futures, so map_instrument builds a SpotInstrument and refuses every other
// market outright rather than filtering rows by type.
//
// Timestamps are ISO-8601 strings, so there is no ms_to_unix; the parser
// below replaces it and lives here because the format is the reason it
// exists.
//
// The aggressor side arrives INVERTED. The matches channel states the maker's
// direction, so map_trade flips it — the opposite of okx, whose `side` is
// already the taker's.

#include <raikiri/domain/market_data.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/coinbase/wire.hpp>

#include <optional>

namespace raikiri::venues::coinbase {

// "2026-08-10T20:51:42.507746Z" -> UnixNanos. Returns nullopt for anything
// that is not that shape; callers substitute ingest time rather than zero.
[[nodiscard]] std::optional<UnixNanos> parse_iso8601(std::string_view text) noexcept;

// A missing or unreadable venue timestamp yields local ingest time: a zero
// propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos iso_to_unix(std::string_view text, UnixNanos fallback) noexcept;

// Public market data ------------------------------------------------------

// The ticker channel is TRADE-DRIVEN: Coinbase republishes it once per match
// and never on a bare quote change (verified — 21 ticker frames over 20 s on
// BTC-USD joined one-to-one to 21 prints by trade_id). The BBO it carries is
// therefore a snapshot taken at each trade, and quote moves between trades
// are invisible on this feed. ts_event is the triggering match's time because
// that is the only time the frame states.
[[nodiscard]] std::optional<Bbo>
map_bbo(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireMatch& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Builds a domain instrument from a /products row. Returns nullopt for any
// market other than Spot, anything not "online", anything trading_disabled,
// and any row whose increments failed to parse — an instrument with an
// unusable tick size must never enter the table, because every later price
// conversion divides by it.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireProduct& wire, MarketType market);

} // namespace raikiri::venues::coinbase
