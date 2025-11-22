#include <raikiri/venues/bybit/map.hpp>

#include <gtest/gtest.h>

#include <string>

namespace raikiri::venues::bybit {
namespace {

// A table built the way a gateway builds it: from a catalogue entry, so the
// tests exercise the same tick/lot values the mapping will see in production.
exec::InstrumentTable make_table() {
    exec::InstrumentTable table;
    table.reserve(4);
    WireInstrument btc{};
    btc.base_coin = "BTC";
    btc.quote_coin = "USDT";
    btc.symbol = "BTCUSDT";
    btc.status = "Trading";
    btc.tick_size = "0.10";
    btc.qty_step = "0.001";
    btc.min_notional = "5";
    // 480 minutes is bybit's own figure for this instrument, and it has to be
    // said here: the funding memo carries the interval down from the interned
    // perp, so a table built without one describes an instrument whose
    // schedule the venue never stated.
    btc.funding_interval_min = 480;
    const auto instrument = map_instrument(btc, MarketType::Perp);
    EXPECT_TRUE(instrument.has_value());
    EXPECT_TRUE(table.add(*instrument).has_value());
    return table;
}

TEST(BybitMapInstrument, TheSettlementScheduleIsTheVenuesAndNotAConstant) {
    // Bybit settles 412 of its linear perps every eight hours, 411 every four
    // and seven every hour. This mapping answered eight hours for all of them.
    //
    // The cost is not a mislabelled row. An interval is the denominator that
    // makes two venues' rates comparable — rate / (interval / 3600) — so
    // calling a four-hour instrument an eight-hour one reports HALF the funding
    // it actually pays, and ranks it against a genuine eight-hour venue as
    // though the two were the same thing.
    WireInstrument wire{};
    wire.base_coin = "ARK";
    wire.quote_coin = "USDT";
    wire.symbol = "ARKUSDT";
    wire.contract_type = "LinearPerpetual";
    wire.status = "Trading";
    wire.tick_size = "0.00001";
    wire.qty_step = "1";
    wire.min_notional = "5";
    wire.funding_interval_min = 60;
    const auto hourly = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(hourly.has_value());
    EXPECT_EQ(std::get<PerpInstrument>(*hourly).funding_interval_s, 3'600U);

    wire.funding_interval_min = 240;
    const auto four_hourly = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(four_hourly.has_value());
    EXPECT_EQ(std::get<PerpInstrument>(*four_hourly).funding_interval_s, 14'400U);

    // ZERO STAYS ZERO. Forty rows carry it, and downstream reads it as "this
    // venue did not say" — which is the one honest answer. Defaulting it to
    // eight hours here would put a schedule nobody published onto a board that
    // settles money against it.
    wire.funding_interval_min = 0;
    const auto unstated = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(unstated.has_value());
    EXPECT_EQ(std::get<PerpInstrument>(*unstated).funding_interval_s, 0U);
}

TEST(BybitMapInstrument, TheCeilingIsCarriedAndAnAbsentOneStaysUnstated) {
    // A rate resting on its cap is demand the venue stopped pricing, so the
    // next settlement is no evidence about the one after. The caps are not
    // uniform either — bybit holds BTC and ETH to 0.333% while most of the
    // catalogue sits at 2% — so this cannot be a constant.
    WireInstrument wire{};
    wire.base_coin = "BTC";
    wire.quote_coin = "USDT";
    wire.symbol = "BTCUSDT";
    wire.contract_type = "LinearPerpetual";
    wire.status = "Trading";
    wire.tick_size = "0.10";
    wire.qty_step = "0.001";
    wire.min_notional = "5";
    wire.funding_interval_min = 480;
    wire.funding_cap = "0.00333";
    wire.funding_floor = "-0.00333";
    const auto capped = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(capped.has_value());
    EXPECT_EQ(std::get<PerpInstrument>(*capped).funding_cap.raw, 3'330'000);
    EXPECT_EQ(std::get<PerpInstrument>(*capped).funding_floor.raw, -3'330'000);

    // UNDEF, NOT ZERO. A floor of zero is a claim that the venue never pays.
    wire.funding_cap = {};
    wire.funding_floor = {};
    const auto silent = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(silent.has_value());
    EXPECT_TRUE(std::get<PerpInstrument>(*silent).funding_cap.is_undef());
    EXPECT_TRUE(std::get<PerpInstrument>(*silent).funding_floor.is_undef());
}

TEST(BybitMapInstrument, ADatedContractIsNotAPerpetual) {
    // Bybit's linear catalogue carries the whole futures curve beside the
    // perpetual. Interning a dated row as a perp gave it the canonical name of
    // the perp, so one venue's curve read as nine venues quoting one market at
    // nine prices — a fake arbitrage, from a catalogue nobody had looked at.
    WireInstrument dated{};
    dated.base_coin = "BTC";
    dated.quote_coin = "USDT";
    dated.symbol = "BTCUSDT-25DEC26";
    dated.contract_type = "LinearFutures";
    dated.status = "Trading";
    dated.tick_size = "0.10";
    dated.qty_step = "0.001";
    EXPECT_FALSE(map_instrument(dated, MarketType::Perp).has_value());

    dated.contract_type = "LinearPerpetual";
    dated.symbol = "BTCUSDT";
    const auto perp = map_instrument(dated, MarketType::Perp);
    ASSERT_TRUE(perp.has_value());
    EXPECT_EQ(base_of(*perp).local_symbol.view(), "BTC/USDT");
}

constexpr UnixNanos k_ingest{1'700'000'000'000'000'000ULL};

TEST(BybitMapInstrument, TradingEntryBecomesAPerp) {
    WireInstrument wire{};
    wire.base_coin = "BTC";
    wire.quote_coin = "USDT";
    wire.symbol = "BTCUSDT";
    wire.status = "Trading";
    wire.tick_size = "0.10";
    wire.qty_step = "0.001";
    wire.min_notional = "5";

    const auto instrument = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    EXPECT_TRUE(std::holds_alternative<PerpInstrument>(*instrument));
    const auto& base = base_of(*instrument);
    EXPECT_EQ(base.exchange_symbol.view(), "BTCUSDT");
    EXPECT_EQ(base.tick_size.raw, 100'000'000); // 0.10 * 1e9
    EXPECT_EQ(base.lot_size.raw, 1'000'000);    // 0.001 * 1e9
}

TEST(BybitMapInstrument, UntradableAndUnusableEntriesAreRefused) {
    WireInstrument wire{};
    wire.base_coin = "BTC";
    wire.quote_coin = "USDT";
    wire.symbol = "BTCUSDT";
    wire.tick_size = "0.10";
    wire.qty_step = "0.001";

    wire.status = "PreLaunch";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());

    // An instrument with no usable tick size divides by zero at order time;
    // it must never enter the table.
    wire.status = "Trading";
    wire.tick_size = "";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
    wire.tick_size = "0";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
}

TEST(BybitInstrumentTable, InterningAssignsIndexableIds) {
    auto table = make_table();
    ASSERT_EQ(table.size(), 1U);

    const auto* found = table.find(std::string_view{"BTCUSDT"});
    ASSERT_NE(found, nullptr);
    const auto id = base_of(*found).id;
    EXPECT_EQ(id.venue(), VenueId::Bybit);
    EXPECT_EQ(id.market(), MarketType::Perp);
    EXPECT_EQ(id.symbol().raw, 0U) << "first instrument takes slot 0";

    // Round trip: the id indexes straight back, and the venue's own text is
    // recoverable for outbound orders.
    EXPECT_EQ(table.find(id), found);
    EXPECT_EQ(table.exchange_symbol(id), "BTCUSDT");
    EXPECT_EQ(table.find(std::string_view{"ETHUSDT"}), nullptr);
}

TEST(BybitInstrumentTable, DuplicatesAndInvalidsRefused) {
    auto table = make_table();
    WireInstrument dup{};
    dup.base_coin = "BTC";
    dup.quote_coin = "USDT";
    dup.symbol = "BTCUSDT";
    dup.status = "Trading";
    dup.tick_size = "0.10";
    dup.qty_step = "0.001";
    const auto again = map_instrument(dup, MarketType::Perp);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(table.add(*again).error(), exec::TableError::DuplicateSymbol);
}

TEST(BybitMapTrade, ConvertsExactlyAndCarriesAggressor) {
    const auto table = make_table();
    WireTrade wire{};
    wire.symbol = "BTCUSDT";
    wire.price = "16578.50";
    wire.size = "0.001";
    wire.buyer_aggressor = true;
    wire.ts_ms = 1'672'304'486'865ULL;

    const auto trade = map_trade(wire, table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->price.raw, 165'785); // 16578.50 / 0.10
    EXPECT_EQ(trade->qty.raw, 1);         // 0.001 / 0.001
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->ts_event.ns, 1'672'304'486'865'000'000ULL);
    EXPECT_EQ(trade->ts_init.ns, k_ingest.ns);
}

TEST(BybitMapTrade, UnknownSymbolAndOffGridPriceAreDropped) {
    const auto table = make_table();
    WireTrade wire{};
    wire.symbol = "DOGEUSDT"; // never subscribed
    wire.price = "1.0";
    wire.size = "0.001";
    EXPECT_FALSE(map_trade(wire, table, k_ingest).has_value());

    // Off the tick grid means bybit changed a filter. Rounding here is how a
    // price ends up one tick from where it was meant to be.
    wire.symbol = "BTCUSDT";
    wire.price = "16578.55";
    EXPECT_FALSE(map_trade(wire, table, k_ingest).has_value());
}

TEST(BybitMapTrade, MissingVenueTimeFallsBackToIngest) {
    const auto table = make_table();
    WireTrade wire{};
    wire.symbol = "BTCUSDT";
    wire.price = "16578.50";
    wire.size = "0.001";
    wire.ts_ms = 0;

    const auto trade = map_trade(wire, table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    // A zero timestamp propagating into latency histograms would poison them.
    EXPECT_EQ(trade->ts_event.ns, k_ingest.ns);
}

TEST(BybitMapFunding, SnapshotPublishesAndEmptyDeltaDoesNot) {
    const auto table = make_table();
    WireTicker wire{};
    wire.symbol = "BTCUSDT";
    wire.is_snapshot = true;
    wire.funding_rate = "-0.000212";
    wire.next_funding_ms = "1673280000000";
    wire.mark_price = "17217.30";
    wire.ts_ms = 1'673'272'861'686ULL;

    FundingMemo memo{};
    const auto funding = map_funding(wire, table, memo, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, -212'000); // -0.000212 * 1e9
    EXPECT_EQ(funding->mark_price.raw, 172'173);
    EXPECT_EQ(funding->next_funding_time.ns, 1'673'280'000'000'000'000ULL);
    EXPECT_EQ(funding->interval_s, 28'800U);

    // A delta that changed only the touch carries no funding news. Publishing
    // one would overwrite a live rate with a default — the exact failure the
    // wire layer keeps absent fields empty to prevent.
    WireTicker delta{};
    delta.symbol = "BTCUSDT";
    delta.bid1_price = "17215.00";
    EXPECT_FALSE(map_funding(delta, table, memo, k_ingest).has_value());
}

TEST(BybitMapFunding, AMarkOnlyFrameStillPublishes) {
    // Measured on BTCUSDT over fifty seconds: one frame carried both the rate
    // and the mark, FORTY-THREE carried only the mark, none carried only the
    // rate. Refusing a mark-only frame threw away every one of the forty-three.
    const auto table = make_table();
    FundingMemo memo{};

    WireTicker snapshot{};
    snapshot.symbol = "BTCUSDT";
    snapshot.is_snapshot = true;
    snapshot.funding_rate = "-0.000212";
    snapshot.mark_price = "17217.30";
    ASSERT_TRUE(map_funding(snapshot, table, memo, k_ingest).has_value());

    WireTicker mark_only{};
    mark_only.symbol = "BTCUSDT";
    mark_only.mark_price = "17220.10";
    const auto merged = map_funding(mark_only, table, memo, k_ingest);
    ASSERT_TRUE(merged.has_value()) << "a new mark is news even with no rate beside it";
    EXPECT_EQ(merged->mark_price.raw, 172'201);
    EXPECT_EQ(merged->rate.raw, -212'000) << "the rate the snapshot stated is carried forward";
}

TEST(BybitMapFunding, ARateOnlyFrameDoesNotEraseTheMark) {
    // The other half of the same fault: publishing straight from a frame that
    // states a rate and no mark writes UNDEF over the last known mark, and the
    // shm cell is written whole.
    const auto table = make_table();
    FundingMemo memo{};

    WireTicker snapshot{};
    snapshot.symbol = "BTCUSDT";
    snapshot.is_snapshot = true;
    snapshot.funding_rate = "-0.000212";
    snapshot.mark_price = "17217.30";
    ASSERT_TRUE(map_funding(snapshot, table, memo, k_ingest).has_value());

    WireTicker rate_only{};
    rate_only.symbol = "BTCUSDT";
    rate_only.funding_rate = "-0.000100";
    const auto merged = map_funding(rate_only, table, memo, k_ingest);
    ASSERT_TRUE(merged.has_value());
    EXPECT_EQ(merged->rate.raw, -100'000);
    EXPECT_EQ(merged->mark_price.raw, 172'173) << "the mark survives a frame that omitted it";
}

TEST(BybitMapFunding, AFieldNeverSeenStaysUndefRatherThanZero) {
    const auto table = make_table();
    FundingMemo memo{};

    WireTicker mark_only{};
    mark_only.symbol = "BTCUSDT";
    mark_only.mark_price = "17217.30";
    const auto first = map_funding(mark_only, table, memo, k_ingest);
    ASSERT_TRUE(first.has_value());
    EXPECT_TRUE(first->rate.is_undef())
        << "no rate has ever been stated; zero would be a rate we invented";
    EXPECT_EQ(first->next_funding_time.ns, 0U);
}

TEST(BybitMapTickerStats, TurnoverPublishesAndAnAbsentColumnDoesNot) {
    const auto table = make_table();
    WireTicker wire{};
    wire.symbol = "BTCUSDT";
    wire.is_snapshot = true;
    wire.volume_24h = "91705.276";
    wire.turnover_24h = "1570383121.943499";
    wire.ts_ms = 1'673'272'861'686ULL;

    const auto stats = map_ticker_stats(wire, table, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base.raw, 91'705'276'000'000LL);
    EXPECT_EQ(stats->volume_24h_quote.raw, 1'570'383'121'943'499'000LL);

    // The same trap funding has: a delta that restated neither column must
    // publish nothing, or the board reads a day's turnover as zero until the
    // next snapshot arrives.
    WireTicker delta{};
    delta.symbol = "BTCUSDT";
    delta.bid1_price = "17215.00";
    EXPECT_FALSE(map_ticker_stats(delta, table, k_ingest).has_value());
}

TEST(BybitMapOrderEvent, LifecycleTransitions) {
    const auto table = make_table();
    WireOrderUpdate wire{};
    wire.symbol = "BTCUSDT";
    wire.order_id = "5cf98598-4a94";
    wire.order_link_id = "01ARZ3NDEKTSV4RRFFQ69G5FAV";
    wire.side = "Buy";
    wire.order_type = "Limit";
    wire.price = "16000.00";
    wire.qty = "0.001";
    wire.leaves_qty = "0.001";
    wire.updated_ms = 1'672'364'262'457ULL;

    wire.status = WireOrderStatus::New;
    auto event = map_order_event(wire, table, k_ingest);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::Accepted);
    EXPECT_EQ(event->exchange_client_order_id.view(), "01ARZ3NDEKTSV4RRFFQ69G5FAV");
    EXPECT_EQ(event->exchange_order_id.view(), "5cf98598-4a94");
    EXPECT_EQ(event->price.raw, 160'000);
    EXPECT_EQ(event->qty.raw, 1);

    wire.status = WireOrderStatus::Rejected;
    event = map_order_event(wire, table, k_ingest);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::Rejected);

    wire.status = WireOrderStatus::Cancelled;
    event = map_order_event(wire, table, k_ingest);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::Canceled);
}

TEST(BybitMapOrderEvent, FillStatusesEmitNothingBecauseExecutionsOwnFills) {
    // The load-bearing rule: bybit reports every fill twice, once on the
    // execution stream and once as a new cumulative quantity on the order
    // stream. Emitting from both doubles every position in the system.
    const auto table = make_table();
    WireOrderUpdate wire{};
    wire.symbol = "BTCUSDT";
    wire.side = "Buy";
    wire.qty = "0.001";
    wire.leaves_qty = "0";

    wire.status = WireOrderStatus::Filled;
    EXPECT_FALSE(map_order_event(wire, table, k_ingest).has_value());
    wire.status = WireOrderStatus::PartiallyFilled;
    EXPECT_FALSE(map_order_event(wire, table, k_ingest).has_value());
}

TEST(BybitMapOrderEvent, PartiallyFilledCanceledIsTerminalCancel) {
    // Reads like a partial fill, is terminal: the remainder was cancelled and
    // its fills already arrived on the execution stream. Mapping it to a fill
    // status would leave the order open in the store forever.
    const auto table = make_table();
    WireOrderUpdate wire{};
    wire.symbol = "BTCUSDT";
    wire.side = "Buy";
    wire.qty = "0.001";
    wire.leaves_qty = "0";
    wire.status = WireOrderStatus::PartiallyFilledCanceled;

    const auto event = map_order_event(wire, table, k_ingest);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::Canceled);
}

TEST(BybitMapExecution, FillCarriesEverythingTheFsmNeeds) {
    const auto table = make_table();
    WireExecution wire{};
    wire.symbol = "BTCUSDT";
    wire.exec_id = "7e2ae69c-4edf";
    wire.order_id = "f6e324ff-99c2";
    wire.order_link_id = "01ARZ3NDEKTSV4RRFFQ69G5FAV";
    wire.side = "Buy";
    wire.exec_price = "16618.20";
    wire.exec_qty = "0.012";
    wire.exec_fee = "0.005061";
    wire.leaves_qty = "0";
    wire.is_maker = true;
    wire.exec_ms = 1'672'364'174'443ULL;

    const auto event = map_execution(wire, table, k_ingest);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::Filled);
    EXPECT_EQ(event->price.raw, 166'182);
    EXPECT_EQ(event->qty.raw, 12);
    EXPECT_EQ(event->leaves_qty.raw, 0) << "zero leaves is what makes the FSM call it full";
    EXPECT_EQ(event->fee.raw, 5'061'000);
    EXPECT_EQ(event->liquidity, Liquidity::Maker);
    EXPECT_EQ(event->exec_id.view(), "7e2ae69c-4edf");
    EXPECT_EQ(event->ts_event.ns, 1'672'364'174'443'000'000ULL);

    wire.is_maker = false;
    EXPECT_EQ(map_execution(wire, table, k_ingest)->liquidity, Liquidity::Taker);
}

TEST(BybitMapExecution, InexactFillIsDroppedRatherThanApproximated) {
    // A fill we cannot express exactly would enter the position at a wrong
    // size. Dropping it leaves reconciliation to find it, which is recoverable.
    const auto table = make_table();
    WireExecution wire{};
    wire.symbol = "BTCUSDT";
    wire.exec_id = "e1";
    wire.side = "Buy";
    wire.exec_price = "16618.25"; // off the 0.10 tick grid
    wire.exec_qty = "0.012";
    EXPECT_FALSE(map_execution(wire, table, k_ingest).has_value());
}

TEST(BybitMapReports, OpenOrdersAndPositions) {
    const auto table = make_table();
    WireOpenOrder order{};
    order.symbol = "BTCUSDT";
    order.order_link_id = "01ARZ3NDEKTSV4RRFFQ69G5FAV";
    order.side = "Buy";
    order.price = "16000.00";
    order.qty = "0.010";
    order.leaves_qty = "0.004";
    order.status = WireOrderStatus::PartiallyFilled;

    const auto report = map_open_order(order, table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(report->qty.raw, 10);
    EXPECT_EQ(report->leaves.raw, 4);

    // A terminal order appearing in an "open orders" pull is a race; ignoring
    // the entry is the resolution.
    order.status = WireOrderStatus::Filled;
    EXPECT_FALSE(map_open_order(order, table).has_value());

    WirePosition position{};
    position.symbol = "BTCUSDT";
    position.side = "Sell";
    position.size = "0.010";
    position.avg_price = "16618.20";
    const auto pos = map_position(position, table);
    ASSERT_TRUE(pos.has_value());
    // Signed here so nothing downstream has to remember bybit's convention
    // of reporting magnitude plus a separate side.
    EXPECT_EQ(pos->net_qty.raw, -10);
    EXPECT_EQ(pos->avg_entry.raw, 166'182);

    position.side = "Buy";
    EXPECT_EQ(map_position(position, table)->net_qty.raw, 10);
}

TEST(BybitMapIds, OversizedIdentifiersAreRefusedNotTruncated) {
    // A truncated exchange order id silently matches the wrong order, so an
    // id that does not fit means the event stays unattributed instead.
    const auto table = make_table();
    WireExecution wire{};
    wire.symbol = "BTCUSDT";
    wire.side = "Buy";
    wire.exec_price = "16618.20";
    wire.exec_qty = "0.012";
    wire.exec_id = std::string(64, 'x'); // ExecId holds 40
    EXPECT_FALSE(map_execution(wire, table, k_ingest).has_value());
}

} // namespace
} // namespace raikiri::venues::bybit
