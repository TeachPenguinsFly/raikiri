#pragma once

// Wire types -> domain types.
//
// Sizes are CONTRACTS, not base units. `quanto_multiplier` says how much of
// the base asset one contract is, so contract_value comes from the catalogue
// instead of being pinned to 1 the way USD-M perps allow. The lot step is one
// whole contract: public market data quotes integer sizes on every contract,
// verified live including the `enable_decimal` ones.

#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/gate/account.hpp>
#include <raikiri/venues/gate/book.hpp>
#include <raikiri/venues/gate/catalogue.hpp>
#include <raikiri/venues/gate/market_data.hpp>
#include <raikiri/venues/gate/orders.hpp>
#include <raikiri/venues/gate/positions.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::gate {

// Venue milliseconds -> UnixNanos. A zero or absent venue timestamp yields
// local ingest time: a zero propagating into latency histograms poisons them.
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// Venue SECONDS -> UnixNanos, for the funding fields. Reading one of these as
// milliseconds puts the settlement in 1970; reading a millisecond field as
// seconds puts it 30 millennia out. Both are silent.
[[nodiscard]] UnixNanos s_to_unix(std::uint64_t seconds) noexcept;

// Public market data ------------------------------------------------------

[[nodiscard]] std::optional<Bbo>
map_bbo(const WireBookTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// A whole book straight into the domain, for the spot snapshot channel. The
// futures deltas do NOT come through here: they are folded by Book, which owns
// the seed and the update chain and publishes its own snapshot.
[[nodiscard]] std::optional<BookSnapshot>
map_book(const WireBookUpdate& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Returns nullopt when the frame quotes no rate at all — a contract without
// funding must not be reported as one funding at zero. Unlike okx, mark price
// rides the same channel and is filled in when it lands on the price grid.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// The same, off a /contracts row, which states funding for every contract at once.
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireContract& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// The 24-hour turnover off the same frame. Gate's own `volume_24h` is in
// CONTRACTS and is deliberately not read: it compares with no other venue.
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Returns nullopt for anything delisting or not trading, anything that is not
// a perp, and any row whose steps failed to parse.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market);

// Spot half of the catalogue. Gate states spot precision as DIGIT COUNTS, so
// the steps are 10^-n; a pair asking for more than nine fractional digits is
// refused rather than rounded to what FixedE9 can hold.
[[nodiscard]] std::optional<Instrument> map_currency_pair(const WireCurrencyPair& wire);

// Order entry ------------------------------------------------------------

// The remaining size the order frame stated, in lots. Callers cache it per
// venue order id: a user-trade frame carries the fill but never the leaves,
// and the OMS treats leaves as authoritative.
[[nodiscard]] std::optional<QtyLots> order_leaves(const WireOrder& wire,
                                                  const exec::InstrumentTable& table);

// Lifecycle only. A frame that merely reports a fill or an amend yields
// nullopt: its Filled event comes from the user-trade channel, which is the
// only place the exec id and the fee exist, and emitting from both would
// attribute one fill twice.
[[nodiscard]] std::optional<OrderEvent>
map_order_event(const WireOrder& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// `leaves` and `known_client_id` come from the caller's per-order cache: Gate
// splits one fill across two channels and this one carries neither. A trade
// whose own `text` names our order wins over the cached id.
[[nodiscard]] std::optional<OrderEvent> map_user_trade(const WireUserTrade& wire,
                                                       const exec::InstrumentTable& table,
                                                       QtyLots leaves,
                                                       const ExchangeClientOrderId& known_client_id,
                                                       UnixNanos ts_init);

[[nodiscard]] std::optional<exec::OrderStatusReport>
map_open_order(const WireOrder& wire, const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                               const exec::InstrumentTable& table);

// The futures account, as one balance row. Nullopt when Gate named no currency:
// a balance without an asset is a number with no unit, and the one thing worse
// than not knowing the equity is being told it in nothing.
[[nodiscard]] std::optional<BalanceReport> map_futures_account(const WireFuturesAccount& wire);

} // namespace raikiri::venues::gate
