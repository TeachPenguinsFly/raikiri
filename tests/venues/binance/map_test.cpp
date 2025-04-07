#include <raikiri/venues/binance/errors.hpp>
#include <raikiri/venues/binance/map.hpp>

#include <gtest/gtest.h>

namespace raikiri::venues::binance {
namespace {

// BTCUSDT as binance publishes it: 0.10 tick, 0.001 step.
exec::InstrumentTable table_with_btc() {
    exec::InstrumentTable table;
    WireInstrument wire{};
    wire.symbol = "BTCUSDT";
    wire.contract_type = "PERPETUAL";
    wire.status = "TRADING";
    wire.tick_size = "0.10";
    wire.step_size = "0.001";
    wire.min_notional = "5";
    const auto instrument = map_instrument(wire, MarketType::Perp);
    EXPECT_TRUE(instrument.has_value());
    EXPECT_TRUE(table.add(*instrument).has_value());
    return table;
}

constexpr UnixNanos k_ingest{1'700'000'000'000'000'000ULL};

TEST(BinanceMap, BookTickerBecomesBbo) {
    const auto table = table_with_btc();
    WireBookTicker wire{};
    wire.symbol = "BTCUSDT";
    wire.bid_price = "64488.50";
    wire.bid_qty = "5.560";
    wire.ask_price = "64488.60";
    wire.ask_qty = "4.245";
    wire.transaction_ms = 1'786'375'246'658ULL;
    wire.event_ms = 1'786'375'246'659ULL;

    const auto bbo = map_bbo(wire, table, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 644'885); // 64488.50 / 0.10
    EXPECT_EQ(bbo->ask.raw, 644'886);
    EXPECT_EQ(bbo->bid_qty.raw, 5'560); // 5.560 / 0.001
    EXPECT_EQ(bbo->ask_qty.raw, 4'245);
    // ts_event is T, not E — the moment the book changed.
    EXPECT_EQ(bbo->ts_event.ns, 1'786'375'246'658ULL * 1'000'000ULL);
    EXPECT_EQ(bbo->ts_init.ns, k_ingest.ns);
}

TEST(BinanceMap, PriceOffTheTickIsDroppedNotRounded) {
    // A price that does not divide by the tick means binance changed a filter
    // under us. Rounding would put an order one tick from where it was meant.
    const auto table = table_with_btc();
    WireBookTicker wire{};
    wire.symbol = "BTCUSDT";
    wire.bid_price = "64488.55"; // 0.05 past a 0.10 tick
    wire.bid_qty = "1.000";
    wire.ask_price = "64488.60";
    wire.ask_qty = "1.000";
    EXPECT_FALSE(map_bbo(wire, table, k_ingest).has_value());
}

TEST(BinanceMap, UnknownSymbolIsDropped) {
    const auto table = table_with_btc();
    WireBookTicker wire{};
    wire.symbol = "DOGEUSDT";
    wire.bid_price = "0.10";
    wire.bid_qty = "1.000";
    wire.ask_price = "0.20";
    wire.ask_qty = "1.000";
    EXPECT_FALSE(map_bbo(wire, table, k_ingest).has_value());
}

TEST(BinanceMap, MakerFlagIsInvertedIntoAggressorSide) {
    // This is the field most likely to be read backwards. m == true means the
    // buyer was the maker, so the aggressor SOLD.
    const auto table = table_with_btc();
    WireAggTrade wire{};
    wire.symbol = "BTCUSDT";
    wire.price = "64357.70";
    wire.qty = "0.008";
    wire.trade_ms = 1'786'375'704'122ULL;

    wire.buyer_is_maker = true;
    auto trade = map_trade(wire, table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Sell);

    wire.buyer_is_maker = false;
    trade = map_trade(wire, table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->price.raw, 643'577);
    EXPECT_EQ(trade->qty.raw, 8);
}

TEST(BinanceMap, MissingVenueTimeFallsBackToIngest) {
    const auto table = table_with_btc();
    WireAggTrade wire{};
    wire.symbol = "BTCUSDT";
    wire.price = "64357.70";
    wire.qty = "0.008";
    wire.trade_ms = 0;
    const auto trade = map_trade(wire, table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    // Not 1970: a zero timestamp reaching a latency histogram poisons it.
    EXPECT_EQ(trade->ts_event.ns, k_ingest.ns);
}

TEST(BinanceMap, MarkPriceBecomesFunding) {
    const auto table = table_with_btc();
    WireMarkPrice wire{};
    wire.symbol = "BTCUSDT";
    wire.mark_price = "64456.50000000";
    wire.index_price = "64484.26195652";
    wire.funding_rate = "0.00004806";
    wire.next_funding_ms = 1'786'377'600'000ULL;
    wire.event_ms = 1'786'375'403'002ULL;

    const auto funding = map_funding(wire, table, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 48'060); // 0.00004806 * 1e9
    EXPECT_EQ(funding->next_funding_time.ns, 1'786'377'600'000ULL * 1'000'000ULL);
    EXPECT_EQ(funding->interval_s, 28'800U);
    EXPECT_GT(funding->mark_price.raw, 0);
}

TEST(BinanceMap, MarkPriceOffTheTickStillMaps) {
    // Mark is an index-weighted average and is NOT rounded to the trading
    // tick, so the exact-step rule cannot apply to it — demanding
    // divisibility here would drop every funding frame binance sends.
    const auto table = table_with_btc();
    WireMarkPrice wire{};
    wire.symbol = "BTCUSDT";
    wire.mark_price = "64456.53700000";
    wire.funding_rate = "0.00004806";
    wire.next_funding_ms = 1'786'377'600'000ULL;
    EXPECT_TRUE(map_funding(wire, table, k_ingest).has_value());
}

TEST(BinanceMap, ContractWithoutFundingIsNotReportedAsZero) {
    const auto table = table_with_btc();
    WireMarkPrice wire{};
    wire.symbol = "BTCUSDT";
    wire.mark_price = "64456.50";
    wire.funding_rate = {}; // absent, not "0"
    EXPECT_FALSE(map_funding(wire, table, k_ingest).has_value());
}

TEST(BinanceMap, NonPerpetualAndNonTradingRowsAreRefused) {
    WireInstrument wire{};
    wire.symbol = "XAUUSDT";
    wire.contract_type = "TRADIFI_PERPETUAL";
    wire.status = "TRADING";
    wire.tick_size = "0.10";
    wire.step_size = "0.001";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value())
        << "asking for perps and getting a tradfi-hours contract is a position bug";

    wire.symbol = "OMGUSDT";
    wire.contract_type = "PERPETUAL";
    wire.status = "SETTLING";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
}

TEST(BinanceMap, InstrumentWithoutUsableStepsIsRefused) {
    // Every later conversion divides by these, so a zero step must never
    // reach the table.
    WireInstrument wire{};
    wire.symbol = "BTCUSDT";
    wire.contract_type = "PERPETUAL";
    wire.status = "TRADING";
    wire.tick_size = "0";
    wire.step_size = "0.001";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());

    wire.tick_size = "0.10";
    wire.step_size = {};
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
}

TEST(BinanceErrors, AuthCodesHaltTheVenue) {
    for (const auto code :
         {k_unauthorized, k_invalid_signature, k_bad_api_key_format, k_rejected_key_or_ip}) {
        EXPECT_EQ(classify(code), exec::ErrorClass::AuthFailure) << code;
    }
}

TEST(BinanceErrors, ClockSkewIsRetryableNotTerminal) {
    // -1021 is the single most common false terminal: the request was fine,
    // our clock was not. Resync and resend succeeds.
    EXPECT_EQ(classify(k_invalid_timestamp), exec::ErrorClass::Retryable);
}

TEST(BinanceErrors, ExpiredListenKeyIsRetryableAndNotAuthFailure) {
    EXPECT_EQ(classify(k_invalid_listen_key), exec::ErrorClass::Retryable);
    EXPECT_TRUE(is_listen_key_failure(k_invalid_listen_key));
    EXPECT_FALSE(is_listen_key_failure(k_invalid_signature));
}

TEST(BinanceErrors, RateLimitCodesBackOff) {
    EXPECT_EQ(classify(k_too_many_requests), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(k_too_many_orders), exec::ErrorClass::RateLimited);
}

TEST(BinanceErrors, UnknownCodeIsTerminalByDefault) {
    // Deliberate: an unknown refusal retried at full speed earns an IP ban,
    // while a spuriously terminal order is recovered by reconciliation.
    EXPECT_EQ(classify(-999'999), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_bad_symbol), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_post_only_would_take), exec::ErrorClass::Terminal);
}

TEST(BinanceErrors, BenignCancelRacesAreRecognised) {
    EXPECT_TRUE(is_benign_cancel_failure(k_no_such_order));
    EXPECT_TRUE(is_benign_cancel_failure(k_cancel_rejected));
    EXPECT_FALSE(is_benign_cancel_failure(k_balance_insufficient));
}

// --- private stream -------------------------------------------------------

WireOrderUpdate order_update(std::string_view exec_type, std::string_view status) {
    WireOrderUpdate wire{};
    wire.symbol = "BTCUSDT";
    wire.client_order_id = "01HQ8ZK9M4TESTULID000000AB";
    wire.exec_type = exec_type;
    wire.order_status = status;
    wire.side = "BUY";
    wire.order_type = "LIMIT";
    wire.time_in_force = "GTC";
    wire.order_qty = "0.010";
    wire.order_price = "64488.50";
    wire.last_filled_qty = "0.000";
    wire.cum_filled_qty = "0.000";
    wire.last_filled_price = "0";
    wire.order_id = 4611875134427365377ULL;
    wire.event_ms = 1'786'375'246'659ULL;
    wire.trade_ms = 1'786'375'246'658ULL;
    return wire;
}

TEST(BinanceMap, ExecutionTypeDecidesTheEventNotOrderStatus) {
    // The double-count hazard. A TRADE frame also reports X = FILLED; if the
    // status were turned into an event of its own, every fill would be
    // counted twice and every position would double.
    const auto table = table_with_btc();

    auto wire = order_update("TRADE", "FILLED");
    wire.last_filled_qty = "0.004";
    wire.cum_filled_qty = "0.010";
    wire.last_filled_price = "64488.60";
    wire.commission = "0.01289772";
    wire.is_maker = false;

    const auto event = map_order_event(wire, table, k_ingest);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::Filled);
    // The FILL's own price and size, not the resting order's.
    EXPECT_EQ(event->price.raw, 644'886);
    EXPECT_EQ(event->qty.raw, 4);
    EXPECT_EQ(event->leaves_qty.raw, 0);
    EXPECT_EQ(event->liquidity, 2) << "m == false means we took liquidity";
    EXPECT_GT(event->fee.raw, 0);
}

TEST(BinanceMap, LifecycleTransitionsComeFromExecutionTypeToo) {
    const auto table = table_with_btc();
    for (const auto& [exec_type, expected] : {std::pair{"NEW", OrderEventType::Accepted},
                                              std::pair{"CANCELED", OrderEventType::Canceled},
                                              std::pair{"EXPIRED", OrderEventType::Expired},
                                              std::pair{"AMENDMENT", OrderEventType::Updated}}) {
        const auto event = map_order_event(order_update(exec_type, "NEW"), table, k_ingest);
        ASSERT_TRUE(event.has_value()) << exec_type;
        EXPECT_EQ(event->type, expected) << exec_type;
        // A non-fill frame describes the resting order.
        EXPECT_EQ(event->price.raw, 644'885) << exec_type;
        EXPECT_EQ(event->qty.raw, 10) << exec_type;
    }
}

TEST(BinanceMap, LiquidationBookkeepingProducesNoOrderEvent) {
    // CALCULATED is ADL/liquidation accounting, not a transition of an order
    // we submitted. Synthesizing one would apply a transition the FSM has no
    // arm for and refuse it loudly for no reason.
    const auto table = table_with_btc();
    EXPECT_FALSE(map_order_event(order_update("CALCULATED", "NEW"), table, k_ingest).has_value());
}

TEST(BinanceMap, OrdersWeDidNotMintAreDropped) {
    // Binance mints its own client ids for liquidations ("autoclose-..."),
    // longer than the field reserved for our ULIDs. The store has no such
    // order, so the frame is not ours to apply.
    const auto table = table_with_btc();
    auto wire = order_update("TRADE", "FILLED");
    wire.client_order_id = "autoclose-1234567890123456789012345678901234567890";
    wire.last_filled_qty = "0.004";
    wire.last_filled_price = "64488.60";
    EXPECT_FALSE(map_order_event(wire, table, k_ingest).has_value());
}

TEST(BinanceMap, PartialFillReportsWhatIsLeft) {
    const auto table = table_with_btc();
    auto wire = order_update("TRADE", "PARTIALLY_FILLED");
    wire.last_filled_qty = "0.004";
    wire.cum_filled_qty = "0.004";
    wire.last_filled_price = "64488.60";
    const auto event = map_order_event(wire, table, k_ingest);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->qty.raw, 4);
    EXPECT_EQ(event->leaves_qty.raw, 6) << "0.010 ordered minus 0.004 done";
}

TEST(BinanceMap, PositionAmountKeepsItsSign) {
    // A short is a negative position. Losing the sign here inverts the book.
    const auto table = table_with_btc();
    WirePositionRisk wire{};
    wire.symbol = "BTCUSDT";
    wire.position_amt = "-0.015";
    wire.entry_price = "64123.47";
    const auto report = map_position(wire, table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->net_qty.raw, -15);
    EXPECT_GT(report->avg_entry.raw, 0);
}

TEST(BinanceMap, UserTradeBecomesAFillReport) {
    const auto table = table_with_btc();
    WireTrade wire{};
    wire.symbol = "BTCUSDT";
    wire.price = "64488.60";
    wire.qty = "0.004";
    wire.commission = "0.01289772";
    wire.side = "SELL";
    wire.trade_id = 698759;
    wire.order_id = 25851813;
    wire.time_ms = 1'786'375'246'658ULL;
    wire.maker = true;
    const auto report = map_user_trade(wire, table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->side, Side::Sell);
    EXPECT_EQ(report->qty.raw, 4);
    EXPECT_EQ(report->liquidity, 1) << "maker";
    EXPECT_EQ(report->exec_id.view(), "698759");
}

} // namespace
} // namespace raikiri::venues::binance
