#include <raikiri/domain/asset_class.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/venues/ostium/map.hpp>
#include <raikiri/venues/ostium/wire.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace raikiri::venues::ostium {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

constexpr UnixNanos k_ingest{1'791'300'060'000'000'000ULL};

WirePair pair(std::string_view symbol) {
    static const auto body = fixture("rest_pairs.json");
    static const auto storage = std::make_unique<PairBuffer>();
    const auto rows = parse_pairs(body, *storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& one : *rows) {
        if (one.symbol == symbol) {
            return one;
        }
    }
    ADD_FAILURE() << "no pair " << symbol;
    return {};
}

InstrumentBase loaded(std::string_view symbol) {
    auto instrument = map_instrument(pair(symbol), MarketType::Perp);
    EXPECT_TRUE(instrument.has_value()) << symbol;
    if (!instrument.has_value()) {
        return {};
    }
    classify(VenueId::Ostium, *instrument);
    return base_of(*instrument);
}

WireRollover rollover(std::string_view id) {
    static const auto body = fixture("rest_subgraph_pairs.json");
    static const auto storage = std::make_unique<RolloverBuffer>();
    const auto rows = parse_rollover(body, *storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& one : *rows) {
        if (one.id == id) {
            return one;
        }
    }
    ADD_FAILURE() << "no subgraph pair " << id;
    return {};
}

TEST(OstiumCatalogue, ListsEveryOnChainPairWithTheVenuesClass) {
    const auto body = fixture("rest_pairs.json");
    const auto storage = std::make_unique<PairBuffer>();
    const auto rows = parse_pairs(body, *storage);
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 83U);
    EXPECT_FALSE(map_instrument((*rows)[0], MarketType::Spot).has_value());

    const auto btc = loaded("BTC-USD");
    EXPECT_EQ(btc.local_symbol.view(), "BTC/USDC");
    EXPECT_EQ(btc.exchange_symbol.view(), "BTC-USD");
    EXPECT_EQ(btc.tick_size.raw, 1);
    EXPECT_EQ(btc.lot_size.raw, 1);
    EXPECT_EQ(btc.kind, ContractKind::Linear);
    EXPECT_EQ(asset_key(btc), "crypto:BTC");

    EXPECT_EQ(asset_key(loaded("NVDA-USD")), "equity:NVDA");
    EXPECT_EQ(asset_key(loaded("URA-USD")), "equity:URA");
    EXPECT_EQ(asset_key(loaded("US500-USD")), "index:SPX");
    EXPECT_EQ(asset_key(loaded("JP225-JPY")), "index:N225");
    EXPECT_EQ(loaded("JP225-JPY").local_symbol.view(), "JP225/JPY");
    EXPECT_EQ(asset_key(loaded("XAU-USD")), "commodity:XAU");
    EXPECT_EQ(asset_key(loaded("XCU-USD")), "commodity:COPPER");
    EXPECT_EQ(asset_key(loaded("WTI-USD")), "commodity:CL");
    EXPECT_EQ(asset_key(loaded("EUR-USD")), "fx:EURUSD");
    EXPECT_EQ(asset_key(loaded("USD-JPY")), "fx:USDJPY");
}

TEST(OstiumQuotes, TheOracleBidAndAskWithNoSize) {
    const auto frame = fixture("ws_tick.json");
    ASSERT_EQ(parse_frame(frame), Frame::Tick);
    QuoteBuffer storage;
    const auto rows = parse_quotes(frame, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1U);
    const auto btc = loaded("BTC-USD");
    const auto bbo = map_bbo((*rows)[0], btc, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 86'527'726'491'497);
    EXPECT_EQ(bbo->ask.raw, 86'548'162'908'502);
    EXPECT_EQ(bbo->bid_qty.raw, 0);
    EXPECT_EQ(bbo->ask_qty.raw, 0);
    EXPECT_EQ(bbo->ts_event.ns, 1'791'300'057'066'000'000ULL);
    EXPECT_EQ(mid_ticks((*rows)[0])->raw, 86'537'944'700'000);
}

TEST(OstiumQuotes, TheSnapshotCarriesEveryFilteredPair) {
    const auto frame = fixture("ws_snapshot.json");
    ASSERT_EQ(parse_frame(frame), Frame::Snapshot);
    QuoteBuffer storage;
    const auto rows = parse_quotes(frame, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);
    EXPECT_EQ((*rows)[1].pair, "EUR-USD");
    const auto bbo = map_bbo((*rows)[1], loaded("EUR-USD"), k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 1'126'380'718);
    EXPECT_EQ(parse_frame(fixture("ws_ack.json")), Frame::Ack);
    EXPECT_EQ(parse_frame(fixture("ws_error.json")), Frame::Error);
}

TEST(OstiumFunding, RolloverIsWhatALongPaysPerHourAccruedEveryBlock) {
    const auto btc = loaded("BTC-USD");
    const auto row = rollover("0");
    const auto funding = map_funding(row, btc, PriceTicks::from_raw(86'537'944'700'000), k_ingest);
    ASSERT_TRUE(funding.has_value());
    // (283593750 + 238000000) per block x 14400 blocks, over 1e18
    EXPECT_EQ(funding->rate.raw, 7'510);
    EXPECT_TRUE(funding->predicted_rate.is_undef());
    EXPECT_EQ(funding->interval_s, 3600U);
    EXPECT_EQ(funding->accrual, FundingAccrual::Continuous);
    EXPECT_EQ(funding->next_funding_time.ns, 0U);
    EXPECT_EQ(funding->ts_event.ns, 0U);
    EXPECT_EQ(funding->mark_price.raw, 86'537'944'700'000);
    EXPECT_EQ(funding->index_price.raw, 86'537'944'700'000);

    // usd-cad carries negative: a long is paid
    EXPECT_EQ(long_rate_per_hour(rollover("16"))->raw, -363);
}

TEST(OstiumFunding, NegativeRolloverFloorsAtZeroWhereThePairForbidsIt) {
    WireRollover row{.id = "1",
                     .long_oi = "0",
                     .short_oi = "0",
                     .long_pure = "-500000000",
                     .premium = "100000000",
                     .negative_allowed = false};
    EXPECT_EQ(long_rate_per_hour(row)->raw, 0);
    row.negative_allowed = true;
    EXPECT_EQ(long_rate_per_hour(row)->raw, -5'760);
}

TEST(OstiumTicker, OpenInterestIsBothSides) {
    const auto btc = loaded("BTC-USD");
    const auto stats = map_ticker(
        rollover("0"), btc, FixedE9::from_raw(2'000'000'000), FixedE6::from_raw(173'000'000'000), k_ingest);
    EXPECT_EQ(stats.open_interest.raw, 1'131'759'683 + 607'572'899);
    EXPECT_EQ(stats.volume_24h_quote.raw, 173'000'000'000);
    EXPECT_EQ(stats.ts_event.ns, 0U);
}

TEST(OstiumTrades, SubgraphFillsAreTradesAndCollateralEditsAreNot) {
    const auto body = fixture("rest_subgraph_orders.json");
    const auto storage = std::make_unique<FillBuffer>();
    const auto rows = parse_fills(body, *storage);
    ASSERT_TRUE(rows.has_value()) << to_string(rows.error());
    ASSERT_GE(rows->size(), 2U);
    const auto& open = (*rows)[0];
    EXPECT_EQ(open.id, "2212760");
    EXPECT_EQ(open.pair_id, "11");
    const auto base = loaded("BTC-USD");
    const auto print = map_fill(open, base, k_ingest);
    ASSERT_TRUE(print.has_value());
    EXPECT_EQ(print->trade.aggressor, Side::Sell);
    EXPECT_EQ(print->trade.price.raw, 51'503'216'670'000);
    EXPECT_EQ(print->trade.qty.raw, 308'439'958);
    EXPECT_EQ(print->trade.ts_event.ns, 1'791'296'713'000'000'000ULL);
    EXPECT_EQ(print->quote.raw, 15'885'649'986);

    const auto& close = (*rows)[1];
    EXPECT_EQ(close.action, "Close");
    EXPECT_EQ(close.close_percent, 2'500U);
    const auto closed = map_fill(close, base, k_ingest);
    ASSERT_TRUE(closed.has_value());
    EXPECT_EQ(closed->trade.aggressor, Side::Sell);
    EXPECT_EQ(closed->trade.qty.raw, 7'314'265);

    WireFill edit = open;
    edit.action = "TopUpCollateral";
    EXPECT_FALSE(map_fill(edit, base, k_ingest).has_value());
    EXPECT_FALSE(parse_fills(fixture("rest_subgraph_error.json"), *storage).has_value());
}

TEST(OstiumClock, StatusCarriesTheServersMilliseconds) {
    EXPECT_EQ(exec::json_number(fixture("rest_status.json"), "time", 1'000'000), 1'791'300'054'086'000'000ULL);
}

TEST(OstiumCaps, MarketDataOnly) {
    const auto& caps = exec::caps_of(VenueId::Ostium);
    EXPECT_TRUE(caps.markets.perp);
    EXPECT_FALSE(caps.markets.spot);
    EXPECT_FALSE(caps.cancel_by_client_order_id);
    EXPECT_TRUE(caps.ticker_stats);
    EXPECT_EQ(to_string(VenueId::Ostium), "ostium");
    EXPECT_EQ(venue_from_string("ostium"), VenueId::Ostium);
    EXPECT_EQ(venue_from_string(""), VenueId::Invalid);
}

} // namespace
} // namespace raikiri::venues::ostium
