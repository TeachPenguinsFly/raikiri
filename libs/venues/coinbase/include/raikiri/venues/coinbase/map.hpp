#pragma once

// Wire types -> domain types.

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

// Returns nullopt for any market other than Spot, anything not "online",
// anything trading_disabled, and any row whose increments failed to parse.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireProduct& wire, MarketType market);

} // namespace raikiri::venues::coinbase
