#pragma once

// signed amounts become a magnitude plus a side

#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/bitfinex/catalogue.hpp>
#include <raikiri/venues/bitfinex/market_data.hpp>
#include <raikiri/venues/bitfinex/orders.hpp>
#include <raikiri/venues/bitfinex/positions.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::venues::bitfinex {

// bitfinex publishes no step; prices use five significant figures capped at eight decimals, so 1e-8 is the grid
inline constexpr std::int64_t k_step_e9 = 10; // 1e-8 in 1e9 fixed point

// funding settles every eight hours
inline constexpr std::uint32_t k_funding_interval_s = 28800;

// zero or absent yields local ingest time
[[nodiscard]] UnixNanos ms_to_unix(std::uint64_t ms, UnixNanos fallback) noexcept;

// exact decimal to 1e9 fixed point, scientific notation included; trades can carry 1e-8
[[nodiscard]] std::optional<FixedE9> parse_number_e9(std::string_view text) noexcept;

// `symbol` is the venue symbol the channel id was bound to at subscribe time.
[[nodiscard]] std::optional<Bbo> map_bbo(const WireTicker& wire,
                                         std::string_view symbol,
                                         const exec::InstrumentTable& table,
                                         UnixNanos ts_event,
                                         UnixNanos ts_init);

[[nodiscard]] std::optional<TradeTick>
map_trade(const WireTrade& wire, std::string_view symbol, const exec::InstrumentTable& table, UnixNanos ts_init);

// volume is in the base asset; the quote column stays zero as the venue states none
[[nodiscard]] std::optional<TickerStats> map_ticker_stats(const WireTicker& wire,
                                                          std::string_view symbol,
                                                          const exec::InstrumentTable& table,
                                                          UnixNanos ts_event,
                                                          UnixNanos ts_init);

// nullopt when no rate is quoted
[[nodiscard]] std::optional<FundingUpdate> map_funding(const WireDerivStatus& wire,
                                                       std::string_view symbol,
                                                       const exec::InstrumentTable& table,
                                                       UnixNanos ts_init);

// nullopt for another market, paper pairs, or an unparseable row; adds the t prefix
[[nodiscard]] std::optional<Instrument> map_instrument(const WirePairInfo& wire, MarketType market);

// execution rows carry no client id, so it comes from the memo by venue order id
struct OrderContext {
    ExchangeClientOrderId client_order_id{};
    // zero, since a default QtyLots is undef
    QtyLots leaves{QtyLots::from_raw(0)};
};

// nullopt when the record carries no transition; fills come from te/tu
[[nodiscard]] std::optional<OrderEvent> map_order_event(const WireOrder& wire,
                                                        const exec::InstrumentTable& table,
                                                        const ExchangeClientOrderId& client_id,
                                                        UnixNanos ts_init);

[[nodiscard]] std::optional<exec::OrderStatusReport> map_open_order(const WireOrder& wire,
                                                                    const exec::InstrumentTable& table);

[[nodiscard]] std::optional<OrderEvent> map_execution(const WireExecution& wire,
                                                      const exec::InstrumentTable& table,
                                                      const OrderContext& context,
                                                      UnixNanos ts_init);

// nullopt for a closed or flat row; absent means flat
[[nodiscard]] std::optional<exec::PositionReport> map_position(const WirePosition& wire,
                                                               const exec::InstrumentTable& table);

} // namespace raikiri::venues::bitfinex
