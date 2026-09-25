#pragma once

// parse_number_e9 reads both decimal and exponent spellings

#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/kraken/catalogue.hpp>
#include <raikiri/venues/kraken/market_data.hpp>
#include <raikiri/venues/kraken/orders.hpp>
#include <raikiri/venues/kraken/positions.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::kraken {

// hourly on the hour; the ticker never states the period
inline constexpr std::uint32_t k_funding_interval_s = 3600;

// a zero or absent venue time falls back to local ingest time
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// exact decimal or exponent token to 1e9 fixed point; refuses digits below 1e-9
[[nodiscard]] std::optional<FixedE9> parse_number_e9(std::string_view text) noexcept;

// precision is negated: 4 means 0.0001, -3 means 1000
[[nodiscard]] std::optional<FixedE9> step_from_precision(std::int64_t precision) noexcept;

// nullopt for a suspended market
[[nodiscard]] std::optional<Bbo> map_bbo(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// reads relative_funding_rate, the only dimensionless one; nullopt when none is quoted
[[nodiscard]] std::optional<FundingUpdate>
map_funding(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// base stays zero on the inverse family, where volume counts contracts
[[nodiscard]] std::optional<TickerStats>
map_ticker_stats(const WireTicker& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

// nullopt for a row not tradeable, not in this market, or with unparsed steps
[[nodiscard]] std::optional<Instrument> map_instrument(const WireInstrument& wire, MarketType market);

// the order size is unfilledSize plus filledSize
[[nodiscard]] std::optional<exec::OrderStatusReport> map_open_order(const WireOpenOrder& wire,
                                                                    const exec::InstrumentTable& table);

[[nodiscard]] std::optional<exec::FillReport> map_rest_fill(const WireFill& wire, const exec::InstrumentTable& table);

// price is an off-grid average and may be reported as none; the size must convert exactly
[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                               const exec::InstrumentTable& table);

// reason alone decides the effect; edits have no reason value and are confirmed from editorder
enum class DeltaEffect : std::uint8_t {
    Accepted,
    Triggered,
    Canceled,
    Rejected,
    // already covered by fills, or an unknown cause
    Silent,
};

[[nodiscard]] DeltaEffect effect_of(std::string_view reason, bool is_cancel) noexcept;

[[nodiscard]] std::optional<OrderEvent>
map_ws_order(const WireWsOrder& wire, OrderEventType type, const exec::InstrumentTable& table, UnixNanos ts_init);

[[nodiscard]] std::optional<OrderEvent>
map_ws_fill(const WireWsFill& wire, const exec::InstrumentTable& table, UnixNanos ts_init);

} // namespace raikiri::venues::kraken
