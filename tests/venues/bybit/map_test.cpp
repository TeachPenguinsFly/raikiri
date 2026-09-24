#include <raikiri/domain/asset_class.hpp>
#include <raikiri/venues/bybit/map.hpp>

#include <gtest/gtest.h>

#include <string>

namespace raikiri::venues::bybit {
namespace {

// built from a catalogue entry the way a gateway builds it
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
    // the funding memo takes the interval from the perp, so the table needs one
    btc.funding_interval_min = 480;
    const auto instrument = map_instrument(btc, MarketType::Perp);
    EXPECT_TRUE(instrument.has_value());
    EXPECT_TRUE(table.add(*instrument).has_value());
    return table;
}

TEST(BybitMapInstrument, TheSettlementScheduleIsTheVenuesAndNotAConstant) {
    // a four-hour instrument must not map to eight hours
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

    // zero means the venue did not state it
    wire.funding_interval_min = 0;
    const auto unstated = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(unstated.has_value());
    EXPECT_EQ(std::get<PerpInstrument>(*unstated).funding_interval_s, 0U);
}

TEST(BybitMapInstrument, TheCeilingIsCarriedAndAnAbsentOneStaysUnstated) {
    // caps differ by symbol
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

    // undef, not zero
    wire.funding_cap = {};
    wire.funding_floor = {};
    const auto silent = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(silent.has_value());
    EXPECT_TRUE(std::get<PerpInstrument>(*silent).funding_cap.is_undef());
    EXPECT_TRUE(std::get<PerpInstrument>(*silent).funding_floor.is_undef());
}

TEST(BybitMapInstrument, ADatedContractIsNotAPerpetual) {
    // dated futures in the linear catalogue must not intern as perps
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

    // an instrument without a usable tick must not enter the table
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

    // round trip: the id indexes back and the venue text is recoverable
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

    // off the tick grid means bybit changed a filter
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
    // a zero timestamp falls back to ingest
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

    // a touch-only delta carries no funding
    WireTicker delta{};
    delta.symbol = "BTCUSDT";
    delta.bid1_price = "17215.00";
    EXPECT_FALSE(map_funding(delta, table, memo, k_ingest).has_value());
}

TEST(BybitMapFunding, AMarkOnlyFrameStillPublishes) {
    // mark-only frames must still publish
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
    // a rate-only frame must not write undef over the last mark
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
    EXPECT_EQ(stats->volume_24h_quote.raw, 1'570'383'121'943'499LL);

    // a delta restating neither column publishes nothing
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
    // bybit reports each fill on both streams; only the execution stream emits
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
    // PartiallyFilledCanceled is terminal; its fills came on the execution stream
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
    // an inexact fill is dropped; reconciliation finds it
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
    // signed here; bybit reports magnitude plus a separate side
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

TEST(BybitMapInstrument, SymbolTypeSetsTheAssetClass) {
    const auto classified = [](std::string_view symbol,
                               std::string_view base_coin,
                               std::string_view symbol_type) {
        WireInstrument wire{};
        wire.symbol = symbol;
        wire.base_coin = base_coin;
        wire.quote_coin = "USDT";
        wire.contract_type = "LinearPerpetual";
        wire.symbol_type = symbol_type;
        wire.status = "Trading";
        wire.tick_size = "0.01";
        wire.qty_step = "0.001";
        wire.min_notional = "5";
        wire.funding_interval_min = 480;
        auto instrument = map_instrument(wire, MarketType::Perp);
        EXPECT_TRUE(instrument.has_value());
        if (!instrument.has_value()) {
            return InstrumentBase{};
        }
        classify(VenueId::Bybit, *instrument);
        return base_of(*instrument);
    };

    const auto tsla = classified("TSLAUSDT", "TSLA", "stock");
    EXPECT_EQ(tsla.asset_class, AssetClass::Equity);
    EXPECT_EQ(tsla.underlying.view(), "TSLA");

    const auto amd = classified("AMDSTOCKUSDT", "AMDSTOCK", "stock");
    EXPECT_EQ(amd.underlying.view(), "AMD");

    const auto xau = classified("XAUUSDT", "XAU", "commodity");
    EXPECT_EQ(xau.asset_class, AssetClass::Commodity);
    EXPECT_EQ(xau.underlying.view(), "XAU");

    const auto spx = classified("SPXUSDT", "SPX", "");
    EXPECT_EQ(spx.asset_class, AssetClass::Crypto);
    EXPECT_EQ(spx.underlying.view(), "SPX");

    EXPECT_EQ(classified("BONDUSDT", "BOND", "bond").asset_class, AssetClass::Unknown);
}

} // namespace
} // namespace raikiri::venues::bybit
