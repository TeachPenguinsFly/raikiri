#pragma once

// steps derive from priceScale and volScale, and scales above 9 are
// refused; funding comes from the ticker

#include <raikiri/domain/ledger_entry.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/mexc/account.hpp>
#include <raikiri/venues/mexc/book.hpp>
#include <raikiri/venues/mexc/catalogue.hpp>
#include <raikiri/venues/mexc/market_data.hpp>
#include <raikiri/venues/mexc/orders.hpp>
#include <raikiri/venues/mexc/positions.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::mexc {

// zero or absent yields the fallback ingest time
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// 10^-scale by integer division; nullopt past nine digits
[[nodiscard]] std::optional<FixedE9> step_from_scale(std::int64_t scale) noexcept;

// snapshots only; a push.depth delta's first entry is not the touch
[[nodiscard]] std::optional<Bbo> map_bbo(const WireDepth& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// snapshots only; the mapper keeps no book state
[[nodiscard]] std::optional<BookSnapshot>
map_book(const WireDepth& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// futures sizes are contracts and spot sizes base units, so the mappers stay separate
[[nodiscard]] std::optional<Bbo> map_spot_bbo(const StreamQuote& wire,
                                              std::string_view symbol,
                                              std::uint64_t ts_ms,
                                              const exec::InstrumentTable& table,
                                              UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_spot_trade(const StreamTrade& wire, std::string_view symbol, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<BookSnapshot> map_spot_book(const StreamBook& wire,
                                                        std::string_view symbol,
                                                        std::uint64_t ts_ms,
                                                        const exec::InstrumentTable& table,
                                                        UnixNanos ts_init);

// nullopt when the frame quotes no rate; interval_s comes from the catalogue
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// the rest form, which carries the schedule
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireFundingRate& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// base volume is `volume24` contracts times the contract size
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TickerStats>
map_spot_ticker_stats(const WireSpotTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt when not enabled, not served here, or the scales cannot become steps
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market);

// refuses a row not trading or with a step finer than FixedE9
[[nodiscard]] std::optional<Instrument> map_spot_instrument(const WireSpotSymbol& wire);

// futures `side` is open/close: 1 and 2 buy, 3 and 4 sell
[[nodiscard]] Side side_from_futures(std::int64_t side) noexcept;

// nullopt for a working order with dealt volume; the deal frame reports the fill
[[nodiscard]] std::optional<OrderEvent>
map_futures_order_event(const WireFuturesOrder& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// `leaves` comes from the caller since the deal frame omits it
[[nodiscard]] std::optional<OrderEvent> map_futures_deal(const WireFuturesDeal& wire,
                                                         const exec::InstrumentTable& table,
                                                         QtyLots leaves,
                                                         const ExchangeClientOrderId& client_id,
                                                         UnixNanos ts_init);

[[nodiscard]] std::optional<exec::OrderStatusReport> map_futures_open_order(const WireFuturesOrder& wire,
                                                                            const exec::InstrumentTable& table);

// nullopt for a closed position; a size-0 row means flat
[[nodiscard]] std::optional<exec::PositionReport> map_futures_position(const WireFuturesPosition& wire,
                                                                       const exec::InstrumentTable& table);

[[nodiscard]] std::optional<OrderEvent>
map_spot_order_event(const WireSpotOrder& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<exec::OrderStatusReport> map_spot_open_order(const WireSpotOrder& wire,
                                                                         const exec::InstrumentTable& table);

// `is_buyer` is the account's side, and `leaves` comes from the caller
[[nodiscard]] std::optional<OrderEvent>
map_spot_trade(const WireSpotTrade& wire, const exec::InstrumentTable& table, QtyLots leaves, UnixNanos ts_init);

// direction comes from `positionType` and `rate` rather than the
// amount's sign; nullopt for a zero rate
[[nodiscard]] std::optional<LedgerEntry>
map_funding_record(const WireFundingRecord& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

} // namespace raikiri::venues::mexc
