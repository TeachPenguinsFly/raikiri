#pragma once

#include <raikiri/domain/ledger_entry.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/bitget/account.hpp>
#include <raikiri/venues/bitget/book.hpp>
#include <raikiri/venues/bitget/catalogue.hpp>
#include <raikiri/venues/bitget/market_data.hpp>
#include <raikiri/venues/bitget/orders.hpp>
#include <raikiri/venues/bitget/positions.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::bitget {

// a zero or absent venue time falls back to local ingest time
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// both read the same ticker frame
[[nodiscard]] std::optional<Bbo> map_bbo(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt when the frame quotes no rate
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// usdtVolume is not read; base and quote come from baseVolume and quoteVolume
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// snapshots only; books15 restates the top of book on every push
[[nodiscard]] std::optional<BookSnapshot>
map_book(const WireBooks& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] Side side_from(std::string_view text) noexcept;

// a push with an inline trade becomes Filled; a status change alone becomes the lifecycle event
[[nodiscard]] std::optional<OrderEvent>
map_order_update(const WireOrderUpdate& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt for an order that is no longer working
[[nodiscard]] std::optional<exec::OrderStatusReport> map_open_order(const WireOrderUpdate& wire,
                                                                    const exec::InstrumentTable& table);

// neither fills endpoint carries a clientOid, so this joins by venue order id
[[nodiscard]] std::optional<exec::FillReport> map_fill_report(const WireFill& wire, const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                               const exec::InstrumentTable& table);

// only contract_settle_fee rows, amount taken as signed; nullopt for an unreadable amount
[[nodiscard]] std::optional<LedgerEntry>
map_bill(const WireBill& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt for a row not tradable, not in this market, or with unparsed steps
[[nodiscard]] std::optional<Instrument> map_instrument(const WireContract& wire, MarketType market);

[[nodiscard]] std::optional<Instrument> map_instrument(const WireSpotSymbol& wire);

} // namespace raikiri::venues::bitget
