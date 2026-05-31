#pragma once

// Wire types -> domain types.

#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/kucoin/account.hpp>
#include <raikiri/venues/kucoin/book.hpp>
#include <raikiri/venues/kucoin/catalogue.hpp>
#include <raikiri/venues/kucoin/market_data.hpp>
#include <raikiri/venues/kucoin/orders.hpp>
#include <raikiri/venues/kucoin/positions.hpp>

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

// A whole book straight into the domain, for the spot snapshot channel. The
// futures level2 changes do NOT come through here: they are folded by Book,
// which owns the seed and publishes its own snapshot.
[[nodiscard]] std::optional<BookSnapshot>
map_book(const WireBookSnapshot& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// The 24-hour window off a CATALOGUE row. Futures only: kucoin's spot
// tickers carry their own turnover under different names and this adapter does
// not read them, so a spot instrument has no volume rather than a zero.
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireContract& wire, const exec::InstrumentTable& table, UnixNanos ts_init);
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireSpotTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// `symbol` overrides the DTO's own, because the REST form names the funding
// index rather than the contract. Returns nullopt when the frame quotes no
// rate at all — a contract without funding must not be reported as one
// funding at zero. The memo carries what the mark subject has already said,
// because the record is written whole.
[[nodiscard]] std::optional<FundingUpdate> map_funding(const WireFundingRate& wire,
                                                       std::string_view symbol,
                                                       const exec::InstrumentTable& table,
                                                       FundingMemo& memo,
                                                       UnixNanos ts_init);

// The mark arrives under its own subject on the same topic. Published as the
// same record, merged with whatever the funding subject has already said.
[[nodiscard]] std::optional<FundingUpdate> map_mark_price(const WireMarkPrice& wire,
                                                          const exec::InstrumentTable& table,
                                                          FundingMemo& memo,
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
[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                               const exec::InstrumentTable& table);

// The futures wallet, as one balance row. Nullopt when KuCoin named no
// currency: a balance without an asset is a number with no unit.
//
// `total` is marginBalance, NOT accountEquity. KuCoin's equity is
// marginBalance plus unrealisedPNL, and this row already carries the pnl on
// its own field — a caller adding the two would count it twice, and one that
// does not would read an equity figure where it asked for a wallet.
//
// `ts_init` stands in for a venue clock this body does not have: the overview
// states no timestamp at all, and a zero here poisons every latency histogram
// downstream, as ms_to_unix's fallback exists to say.
[[nodiscard]] std::optional<BalanceReport> map_account_overview(const WireAccountOverview& wire,
                                                                UnixNanos ts_init);

// Catalogue ---------------------------------------------------------------

// Returns nullopt for anything not Open, anything that is not a perpetual,
// anything inverse, and any row whose steps failed to parse.
[[nodiscard]] std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market);

// The spot catalogue lives on the other host and states its steps as
// increments rather than as tick/lot sizes.
[[nodiscard]] std::optional<Instrument> map_spot_symbol(const WireSpotSymbol& wire);

} // namespace raikiri::venues::kucoin
