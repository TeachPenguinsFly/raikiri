#pragma once

#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/ledger_entry.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/weex/account.hpp>
#include <raikiri/venues/weex/catalogue.hpp>
#include <raikiri/venues/weex/market_data.hpp>
#include <raikiri/venues/weex/orders.hpp>
#include <raikiri/venues/weex/positions.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace raikiri::venues::weex {

// a zero or absent venue timestamp yields ingest time
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// the frame names the symbol once for several prints
[[nodiscard]] std::optional<TradeTick>
map_trade(const WirePrint& wire, std::string_view symbol, const exec::InstrumentTable& table, UnixNanos ts_init);

// a column the frame omits stays undef
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt when no rate is quoted, since zero is a real rate
// truncating reader: weex pads rates to eleven digits
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WirePremium& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] Side side_from(std::string_view text) noexcept;

// leaves is origQty minus executedQty; a half-filled order is still NEW
[[nodiscard]] std::optional<exec::OrderStatusReport> map_open_order(const WireOrder& wire,
                                                                    const exec::InstrumentTable& table);

// fills carry no client order id
[[nodiscard]] std::optional<exec::FillReport> map_fill(const WireFill& wire, const exec::InstrumentTable& table);

// avg_entry stays undef; weex states no entry price
[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                               const exec::InstrumentTable& table);

// netted per instrument, since separated mode sends one row per direction; keeps the latest timestamp
[[nodiscard]] std::vector<exec::PositionReport> map_positions(std::span<const WirePosition> rows,
                                                              const exec::InstrumentTable& table);

// nullopt when no asset is named; rows carry no timestamp
[[nodiscard]] std::optional<BalanceReport> map_balance(const WireBalance& wire, UnixNanos ts_venue);

// nullopt for other bill types, unknown contracts and unparseable amounts
[[nodiscard]] std::optional<LedgerEntry>
map_funding_charge(const WireBill& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt for another market or steps that do not fit fixed point
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market);

} // namespace raikiri::venues::weex
