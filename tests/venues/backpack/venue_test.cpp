#include <raikiri/domain/asset_class.hpp>
#include <raikiri/venues/backpack/book.hpp>
#include <raikiri/venues/backpack/catalogue.hpp>
#include <raikiri/venues/backpack/envelope.hpp>
#include <raikiri/venues/backpack/map.hpp>
#include <raikiri/venues/backpack/market_data.hpp>
#include <raikiri/venues/backpack/rest.hpp>
#include <raikiri/venues/backpack/ws.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::backpack {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

std::string data_of(std::string_view name) {
    const auto frame = fixture(name);
    const auto inner = unwrap(frame);
    EXPECT_TRUE(inner.has_value());
    return std::string{*inner};
}

constexpr UnixNanos k_ingest{1'700'000'000'000'000'000ULL};

exec::InstrumentTable table_for(MarketType market) {
    exec::InstrumentTable table;
    const auto body = fixture("rest_markets.json");
    static MarketBuffer storage;
    const auto rows = parse_markets(body, storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& row : *rows) {
        if (const auto instrument = map_instrument(row, market); instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value());
        }
    }
    return table;
}

TEST(BackpackCatalogue, OneListCarriesBothMarketsAndOnlyOpenRowsMap) {
    MarketBuffer storage;
    const auto body = fixture("rest_markets.json");
    const auto rows = parse_markets(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 5U);

    std::vector<std::string> spot;
    std::vector<std::string> perp;
    for (const auto& row : *rows) {
        if (const auto i = map_instrument(row, MarketType::Spot); i.has_value()) {
            spot.emplace_back(base_of(*i).exchange_symbol.view());
        }
        if (const auto i = map_instrument(row, MarketType::Perp); i.has_value()) {
            perp.emplace_back(base_of(*i).exchange_symbol.view());
        }
        EXPECT_FALSE(map_instrument(row, MarketType::Future).has_value());
    }
    EXPECT_EQ(spot, (std::vector<std::string>{"SOL_USDC"}));
    EXPECT_EQ(perp, (std::vector<std::string>{"SOL_USDC_PERP", "NVDA.US_USDC_PERP"}));
}

TEST(BackpackCatalogue, TheRwaMarketTypeIsTheAssetClass) {
    MarketBuffer storage;
    const auto body = fixture("rest_markets.json");
    const auto rows = parse_markets(body, storage);
    ASSERT_TRUE(rows.has_value());
    const auto classified = [](const WireMarket& row) {
        const auto instrument = map_instrument(row, MarketType::Perp);
        EXPECT_TRUE(instrument.has_value()) << row.symbol;
        auto base = base_of(instrument.value_or(PerpInstrument{}));
        const auto stated = base.asset_class;
        classify(VenueId::Backpack, base);
        return std::pair{stated, base};
    };
    for (const auto& row : *rows) {
        if (row.symbol == "NVDA.US_USDC_PERP") {
            EXPECT_EQ(row.rwa_market_type, "STOCK");
            const auto [stated, nvda] = classified(row);
            EXPECT_EQ(stated, AssetClass::Equity);
            EXPECT_EQ(nvda.underlying.view(), "NVDA");
        }
        if (row.symbol == "SOL_USDC_PERP") {
            EXPECT_TRUE(row.rwa_market_type.empty());
        }
    }

    WireMarket wire{};
    wire.symbol = "SPX_USDC_PERP";
    wire.base_asset = "SPX";
    wire.quote_asset = "USDC";
    wire.market_type = "PERP";
    wire.order_book_state = "Open";
    wire.tick_size = "0.0001";
    wire.step_size = "1";
    const auto [spx_stated, spx] = classified(wire);
    EXPECT_EQ(spx_stated, AssetClass::Crypto);
    EXPECT_EQ(spx.underlying.view(), "SPX");

    wire.symbol = "SPY.US_USDC_PERP";
    wire.base_asset = "SPY.US";
    wire.rwa_market_type = "INDEX";
    EXPECT_EQ(classified(wire).first, AssetClass::Index);

    wire.rwa_market_type = "BOND";
    const auto [odd_stated, odd] = classified(wire);
    EXPECT_EQ(odd_stated, AssetClass::Unknown);
    EXPECT_EQ(odd.asset_class, AssetClass::Unknown);
}

TEST(BackpackCatalogue, TheFundingIntervalIsTheVenuesOwnFigureAndSpotHasNone) {
    MarketBuffer storage;
    const auto body = fixture("rest_markets.json");
    const auto rows = parse_markets(body, storage);
    ASSERT_TRUE(rows.has_value());
    for (const auto& row : *rows) {
        if (row.symbol == "SOL_USDC") {
            EXPECT_EQ(row.funding_interval_ms, 0U); // null on the wire
        }
        if (row.symbol == "SOL_USDC_PERP") {
            EXPECT_EQ(row.funding_interval_ms, 3'600'000U);
            const auto instrument = map_instrument(row, MarketType::Perp);
            ASSERT_TRUE(instrument.has_value());
            const auto& perp = std::get<PerpInstrument>(*instrument);
            EXPECT_EQ(perp.funding_interval_s, 3600U);
            EXPECT_TRUE(perp.funding_cap.is_undef());
            EXPECT_TRUE(perp.funding_floor.is_undef());
            EXPECT_EQ(perp.base.local_symbol.view(), "SOL/USDC");
            EXPECT_TRUE(perp.base.min_notional.is_undef());
        }
    }
}

TEST(BackpackCatalogue, AnIntervalThatIsNotWholeSecondsIsLeftUnstated) {
    WireMarket wire{};
    wire.symbol = "ODD_USDC_PERP";
    wire.base_asset = "ODD";
    wire.quote_asset = "USDC";
    wire.market_type = "PERP";
    wire.order_book_state = "Open";
    wire.tick_size = "0.01";
    wire.step_size = "0.1";
    wire.funding_interval_ms = 3'600'500;
    const auto instrument = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    EXPECT_EQ(std::get<PerpInstrument>(*instrument).funding_interval_s, 0U);
}

TEST(BackpackWire, EveryStreamResolvesToItsKind) {
    for (const auto& [file, kind] : {std::pair{"ws_book_ticker.json", EventKind::BookTicker},
                                     std::pair{"ws_trade.json", EventKind::Trade},
                                     std::pair{"ws_mark_price_perp.json", EventKind::MarkPrice},
                                     std::pair{"ws_mark_price_spot.json", EventKind::MarkPrice},
                                     std::pair{"ws_depth.json", EventKind::Depth}}) {
        EXPECT_EQ(parse_event_kind(data_of(file)), kind) << file;
    }
}

TEST(BackpackMap, BookTickerBecomesBboOnTheMicrosecondClock) {
    const auto table = table_for(MarketType::Spot);
    const auto payload = data_of("ws_book_ticker.json");
    const auto wire = parse_book_ticker(payload);
    ASSERT_TRUE(wire.has_value());
    const auto bbo = map_bbo(*wire, table, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 11'688);
    EXPECT_EQ(bbo->ask.raw, 11'689);
    // T is microseconds
    EXPECT_EQ(bbo->ts_event.ns, 1'790'280'896'113'106'000ULL);
}

TEST(BackpackMap, TradeFlipsTheMakerFlagIntoTheAggressor) {
    const auto table = table_for(MarketType::Perp);
    const auto payload = data_of("ws_trade.json");
    const auto wire = parse_trade(payload);
    ASSERT_TRUE(wire.has_value());
    const auto trade = map_trade(*wire, table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Buy); // m: false, so the buyer took
    EXPECT_EQ(trade->price.raw, 11'682);
    EXPECT_EQ(trade->qty.raw, 24);
}

TEST(BackpackMap, PerpetualMarkPriceCarriesRateNextSettlementAndInterval) {
    const auto table = table_for(MarketType::Perp);
    const auto payload = data_of("ws_mark_price_perp.json");
    const auto wire = parse_mark_price(payload);
    ASSERT_TRUE(wire.has_value());
    const auto funding = map_funding(*wire, table, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 12'500); // 0.0000125
    EXPECT_TRUE(funding->predicted_rate.is_undef());
    EXPECT_EQ(funding->mark_price.raw, 11'682);
    EXPECT_EQ(funding->index_price.raw, 11'686); // 116.8668901 truncated to the cent
    EXPECT_EQ(funding->ts_event.ns, 1'790'280'896'846'962'000ULL);
    // n is milliseconds while E is microseconds
    EXPECT_EQ(funding->next_funding_time.ns, 1'790'283'600'000'000'000ULL);
    EXPECT_EQ(funding->interval_s, 3600U);
}

TEST(BackpackMap, SpotMarkPriceIsNotFundingAtZero) {
    const auto table = table_for(MarketType::Spot);
    const auto payload = data_of("ws_mark_price_spot.json");
    const auto wire = parse_mark_price(payload);
    ASSERT_TRUE(wire.has_value());
    EXPECT_TRUE(wire->funding_rate.empty());
    EXPECT_FALSE(map_funding(*wire, table, k_ingest).has_value());
}

TEST(BackpackMap, ALongRateIsTruncatedRatherThanRefused) {
    const auto table = table_for(MarketType::Perp);
    WireMarkPrice wire{};
    wire.symbol = "SOL_USDC_PERP";
    wire.mark_price = "116.82";
    wire.funding_rate = "-0.0000092211954022829528621616";
    wire.next_funding_ms = 1'790'283'600'000ULL;
    const auto funding = map_funding(wire, table, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, -9'221);
}

TEST(BackpackMap, TickerRowsCarryBothVolumes) {
    const auto table = table_for(MarketType::Spot);
    TickerBuffer storage;
    const auto body = fixture("rest_tickers.json");
    const auto rows = parse_tickers(body, storage);
    ASSERT_TRUE(rows.has_value());
    bool saw = false;
    for (const auto& row : *rows) {
        if (const auto stats = map_ticker_stats(row, table, k_ingest); stats.has_value()) {
            saw = true;
            EXPECT_FALSE(stats->volume_24h_base.is_undef());
            EXPECT_FALSE(stats->volume_24h_quote.is_undef());
            EXPECT_EQ(stats->ts_event.ns, 0U);
        }
    }
    EXPECT_TRUE(saw);
}

TEST(BackpackMap, OpenInterestIsStatedInTheBase) {
    const auto table = table_for(MarketType::Perp);
    OpenInterestBuffer storage;
    const auto body = fixture("rest_open_interest.json");
    const auto rows = parse_open_interest(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 5U);
    const auto* sol = table.find("SOL_USDC_PERP");
    ASSERT_NE(sol, nullptr);
    const auto oi = map_open_interest((*rows)[0], table);
    ASSERT_TRUE(oi.has_value());
    EXPECT_EQ(oi->instrument, base_of(*sol).id);
    EXPECT_EQ(oi->base.raw, 201'981'860'000'000); // 201981.86 SOL
}

TEST(BackpackBook, ConsecutiveDepthFramesChainAsSuccessors) {
    // Frame N+1's `U` must be frame N's `u` + 1.
    LevelBuffer a_buf;
    LevelBuffer b_buf;
    const auto first = data_of("ws_depth.json");
    const auto second = data_of("ws_depth_next.json");
    const auto a = parse_depth_update(first, a_buf);
    const auto b = parse_depth_update(second, b_buf);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->first_id, a->last_id + 1);
}

TEST(BackpackBook, SnapshotReadsItsStringIdAndSeedsABook) {
    const auto table = table_for(MarketType::Perp);
    const auto* sol = table.find("SOL_USDC_PERP");
    ASSERT_NE(sol, nullptr);
    const auto& base = base_of(*sol);

    LevelBuffer storage;
    const auto body = fixture("rest_depth.json");
    const auto snapshot = parse_depth_snapshot(body, storage);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_GT(snapshot->last_update_id, 0U);

    DepthBook book{base.id, base.tick_size, base.lot_size};
    EXPECT_TRUE(book.wants_seed());
    EXPECT_EQ(book.seed(*snapshot), BookResult::Applied);
    ASSERT_TRUE(book.live());

    WireDepthUpdate next{};
    next.symbol = "SOL_USDC_PERP";
    next.first_id = snapshot->last_update_id - 2;
    next.last_id = snapshot->last_update_id + 1;
    EXPECT_EQ(book.apply(next), BookResult::Applied);
    ASSERT_GE(book.bids().size(), 2U);
    EXPECT_GT(book.bids()[0].price.raw, book.bids()[1].price.raw);
    EXPECT_LT(book.bids()[0].price.raw, book.asks()[0].price.raw);

    WireDepthUpdate skipped{};
    skipped.symbol = "SOL_USDC_PERP";
    skipped.first_id = next.last_id + 2;
    skipped.last_id = next.last_id + 2;
    EXPECT_EQ(book.apply(skipped), BookResult::Gap);
    EXPECT_FALSE(book.live());
    EXPECT_TRUE(book.wants_seed());
}

TEST(BackpackMap, ASnapshotTouchIsTheHighestBidWhereverTheVenuePutsIt) {
    const auto table = table_for(MarketType::Perp);
    const auto& base = base_of(*table.find("SOL_USDC_PERP"));
    LevelBuffer storage;
    const auto body = fixture("rest_depth.json");
    const auto snapshot = parse_depth_snapshot(body, storage);
    ASSERT_TRUE(snapshot.has_value());
    const auto bbo = map_touch(*snapshot, base, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 11'672); // 116.72, the last bid on the wire
    EXPECT_EQ(bbo->ask.raw, 11'673); // 116.73, the first ask
    EXPECT_EQ(bbo->bid_qty.raw, 3'347);
    EXPECT_EQ(bbo->ts_event.ns, 1'790'281'845'333'409'000ULL);
}

TEST(BackpackMap, ARestingSizeFinerThanTheStepReadsAsWhatTheStepCanTake) {
    // stepSize 1 with a level of 9.09 resting on it.
    WireMarket wire{};
    wire.symbol = "RAY_USDC";
    wire.base_asset = "RAY";
    wire.quote_asset = "USDC";
    wire.market_type = "SPOT";
    wire.order_book_state = "Open";
    wire.tick_size = "0.0001";
    wire.step_size = "1";
    const auto instrument = map_instrument(wire, MarketType::Spot);
    ASSERT_TRUE(instrument.has_value());
    exec::InstrumentTable table;
    ASSERT_TRUE(table.add(*instrument).has_value());
    const auto& base = base_of(*table.find("RAY_USDC"));

    WireBookTicker quote{};
    quote.symbol = "RAY_USDC";
    quote.bid_price = "2.25";
    quote.bid_qty = "9.09";
    quote.ask_price = "2.2501";
    quote.ask_qty = "0.54"; // smaller than a step: nothing an order can take
    const auto bbo = map_bbo(quote, table, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid_qty.raw, 9);
    EXPECT_EQ(bbo->ask_qty.raw, 0);

    WirePrint print{};
    print.symbol = "RAY_USDC";
    print.price = "2.25";
    print.qty = "9.09";
    EXPECT_FALSE(map_trade(print, table, k_ingest).has_value());

    const std::array<WireLevel, 2> bids{{{.price = "2.2", .qty = "0.54"}, {.price = "2.25", .qty = "9.09"}}};
    const std::array<WireLevel, 1> asks{{{.price = "2.2501", .qty = "3"}}};
    WireDepthSnapshot snapshot{.last_update_id = 10, .timestamp_us = 1, .bids = bids, .asks = asks};
    DepthBook book{base.id, base.tick_size, base.lot_size};
    EXPECT_EQ(book.seed(snapshot), BookResult::Applied);
    ASSERT_EQ(book.bids().size(), 1U); // the 0.54 level has nothing to take
    EXPECT_EQ(book.bids()[0].qty.raw, 9);
}

TEST(BackpackBook, ANewerSeedReplacesALiveBookAndAnOlderOneBouncesOff) {
    const auto table = table_for(MarketType::Perp);
    const auto& base = base_of(*table.find("SOL_USDC_PERP"));
    LevelBuffer storage;
    const auto body = fixture("rest_depth.json");
    auto snapshot = parse_depth_snapshot(body, storage);
    ASSERT_TRUE(snapshot.has_value());
    DepthBook book{base.id, base.tick_size, base.lot_size};
    ASSERT_EQ(book.seed(*snapshot), BookResult::Applied);
    const auto first = snapshot->last_update_id;

    snapshot->last_update_id = first - 1;
    EXPECT_EQ(book.seed(*snapshot), BookResult::Stale);
    EXPECT_EQ(book.last_update_id(), first);

    snapshot->last_update_id = first + 40;
    EXPECT_EQ(book.seed(*snapshot), BookResult::Applied);
    EXPECT_EQ(book.last_update_id(), first + 40);
    EXPECT_TRUE(book.live());
}

TEST(BackpackBook, ABufferedRunThatStartsPastTheSeedAsksForAnother) {
    const auto table = table_for(MarketType::Perp);
    const auto& base = base_of(*table.find("SOL_USDC_PERP"));
    LevelBuffer storage;
    const auto body = fixture("rest_depth.json");
    const auto snapshot = parse_depth_snapshot(body, storage);
    ASSERT_TRUE(snapshot.has_value());
    DepthBook book{base.id, base.tick_size, base.lot_size};
    book.seed_requested();
    WireDepthUpdate later{};
    later.first_id = snapshot->last_update_id + 5;
    later.last_id = snapshot->last_update_id + 6;
    EXPECT_EQ(book.apply(later), BookResult::Buffered);
    // Ids L+1 .. L+4 are in neither the seed nor the run: nothing joins them.
    EXPECT_EQ(book.seed(*snapshot), BookResult::Reseed);
    EXPECT_FALSE(book.live());
    EXPECT_TRUE(book.wants_seed());
}

TEST(BackpackBook, BufferedDeltasReplayOverTheSeed) {
    const auto table = table_for(MarketType::Perp);
    const auto& base = base_of(*table.find("SOL_USDC_PERP"));
    LevelBuffer storage;
    const auto body = fixture("rest_depth.json");
    const auto snapshot = parse_depth_snapshot(body, storage);
    ASSERT_TRUE(snapshot.has_value());
    const auto seed_id = snapshot->last_update_id;

    DepthBook book{base.id, base.tick_size, base.lot_size};
    // One delta wholly before the seed, one straddling it.
    WireDepthUpdate old{};
    old.first_id = seed_id - 3;
    old.last_id = seed_id - 1;
    WireDepthUpdate straddle{};
    straddle.first_id = seed_id;
    straddle.last_id = seed_id + 2;
    book.seed_requested();
    EXPECT_EQ(book.apply(old), BookResult::Buffered);
    EXPECT_EQ(book.apply(straddle), BookResult::Buffered);
    EXPECT_EQ(book.seed(*snapshot), BookResult::Applied);
    EXPECT_EQ(book.last_update_id(), seed_id + 2);
}

TEST(BackpackBook, ALevelOffTheTickDropsTheBookInsteadOfSkippingIt) {
    const auto table = table_for(MarketType::Perp);
    const auto& base = base_of(*table.find("SOL_USDC_PERP"));
    LevelBuffer storage;
    const auto body = fixture("rest_depth.json");
    const auto snapshot = parse_depth_snapshot(body, storage);
    ASSERT_TRUE(snapshot.has_value());
    DepthBook book{base.id, base.tick_size, base.lot_size};
    (void)book.seed(*snapshot);
    const std::array<WireLevel, 1> off_tick{{{.price = "116.805", .qty = "1"}}};
    WireDepthUpdate delta{};
    delta.first_id = snapshot->last_update_id + 1;
    delta.last_id = snapshot->last_update_id + 1;
    delta.bids = off_tick;
    EXPECT_EQ(book.apply(delta), BookResult::Gap);
    EXPECT_FALSE(book.live());
}

TEST(BackpackWire, TheSeedAsksForALimitTheVenueAccepts) {
    RequestBuilder builder;
    const auto request = builder.depth_snapshot("SOL_USDC_PERP", k_seed_depth);
    EXPECT_EQ(request.target, "/api/v1/depth?symbol=SOL_USDC_PERP&limit=100");
}

TEST(BackpackWire, StreamsAreTypeDotSymbolAndDepthIsTheAggregate) {
    std::array<char, 64> buf{};
    EXPECT_EQ(stream_name(exec::SubKind::Bbo, "SOL_USDC", buf), "bookTicker.SOL_USDC");
    EXPECT_EQ(stream_name(exec::SubKind::Depth, "SOL_USDC", buf), "depth.200ms.SOL_USDC");
    EXPECT_EQ(depth_symbol("depth.200ms.SOL_USDC_PERP"), "SOL_USDC_PERP");
    EXPECT_EQ(depth_symbol("bookTicker.SOL_USDC_PERP"), "");
    EXPECT_TRUE(stream_name(exec::SubKind::Funding, "SOL_USDC", buf).empty());

    Protocol protocol;
    const std::array<std::string_view, 2> streams{"bookTicker.SOL_USDC", "trade.SOL_USDC"};
    const auto frame = protocol.subscribe_frame(streams);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->consumed, 2U);
    EXPECT_EQ(frame->text, R"({"method":"SUBSCRIBE","params":["bookTicker.SOL_USDC","trade.SOL_USDC"],"id":1})");
}

TEST(BackpackWire, ARestRefusalNamesItsCodeInWords) {
    const auto body = fixture("rest_error.json");
    const auto err = parse_rest_error(body);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, "INVALID_CLIENT_REQUEST");
    EXPECT_FALSE(err->message.empty());
}

} // namespace
} // namespace raikiri::venues::backpack
