#include <raikiri/exec/caps.hpp>
#include <raikiri/venues/lbank/endpoints.hpp>
#include <raikiri/venues/lbank/map.hpp>
#include <raikiri/venues/lbank/wire.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>

namespace raikiri::venues::lbank {
namespace {

// parsed views point into the text, so every fixture lives for the whole run
const std::string& fixture(std::string_view name) {
    static std::map<std::string, std::string, std::less<>> cache;
    if (const auto it = cache.find(name); it != cache.end()) {
        return it->second;
    }
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return cache.emplace(std::string{name}, buf.str()).first->second;
}

constexpr UnixNanos k_ingest{1'790'964'440'000'000'000ULL};

FixedE9 e9(std::string_view text) {
    return parse_decimal_e9(text).value_or(FixedE9{});
}

InstrumentBase perp_base(std::string_view symbol) {
    static const auto body = fixture("rest_instrument.json");
    static const auto storage = std::make_unique<PerpInstrumentBuffer>();
    const auto rows = parse_perp_instruments(body, *storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& row : *rows) {
        if (row.symbol == symbol) {
            const auto instrument = map_perp_instrument(row, 28'800);
            EXPECT_TRUE(instrument.has_value()) << symbol;
            auto base = base_of(*instrument);
            base.id = InstrumentId::make(VenueId::Lbank, MarketType::Perp, SymbolId{2});
            return base;
        }
    }
    ADD_FAILURE() << "no contract " << symbol;
    return {};
}

InstrumentBase spot_base(std::string_view pair) {
    static const auto body = fixture("rest_accuracy.json");
    static const auto storage = std::make_unique<SpotPairBuffer>();
    const auto rows = parse_spot_pairs(body, *storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& row : *rows) {
        if (row.symbol == pair) {
            const auto instrument = map_spot_instrument(row);
            EXPECT_TRUE(instrument.has_value()) << pair;
            auto base = base_of(*instrument);
            base.id = InstrumentId::make(VenueId::Lbank, MarketType::Spot, SymbolId{4});
            return base;
        }
    }
    ADD_FAILURE() << "no pair " << pair;
    return {};
}

TEST(LbankIdentity, IsVenueThirtyOneWithBothMarketsAndNoOrderEntry) {
    EXPECT_EQ(static_cast<int>(VenueId::Lbank), 31);
    EXPECT_EQ(to_string(VenueId::Lbank), "lbank");
    const auto& caps = exec::caps_of(VenueId::Lbank);
    EXPECT_EQ(caps.venue, VenueId::Lbank);
    EXPECT_TRUE(caps.markets.spot);
    EXPECT_TRUE(caps.markets.perp);
    EXPECT_FALSE(caps.ws_order_entry);
    EXPECT_FALSE(caps.fix_order_entry);
    EXPECT_TRUE(caps.manual_only.perp);
    EXPECT_FALSE(caps.manual_only.spot);
}

TEST(LbankPerpCatalogue, MapsTheTicksAndLeavesTheSuspendedContractOut) {
    const auto body = fixture("rest_instrument.json");
    const auto storage = std::make_unique<PerpInstrumentBuffer>();
    const auto rows = parse_perp_instruments(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);
    std::size_t loaded = 0;
    for (const auto& row : *rows) {
        loaded += map_perp_instrument(row, 28'800).has_value() ? 1U : 0U;
        if (row.symbol == "CEGUSDT") {
            EXPECT_TRUE(row.suspended);
        }
    }
    EXPECT_EQ(loaded, 3U);

    const auto btc = perp_base("BTCUSDT");
    EXPECT_EQ(btc.tick_size, e9("0.1"));
    EXPECT_EQ(btc.lot_size, e9("0.0001"));
    EXPECT_EQ(btc.min_notional, e9("1"));
    EXPECT_EQ(btc.contract_value, e9("1"));
    EXPECT_EQ(btc.kind, ContractKind::Linear);
    EXPECT_EQ(btc.local_symbol.view(), "BTC/USDT");
    EXPECT_EQ(btc.exchange_symbol.view(), "BTCUSDT");
}

TEST(LbankPerpCatalogue, ReadsTheFundingIntervalAndTheContractWithoutARate) {
    const auto body = fixture("rest_market_data.json");
    const auto storage = std::make_unique<PerpMarketBuffer>();
    const auto rows = parse_perp_markets(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);
    for (const auto& row : *rows) {
        EXPECT_EQ(row.interval_s, 28'800U) << row.symbol;
        EXPECT_EQ(row.next_fee_ms, 1'790'985'600'000ULL) << row.symbol;
    }
}

TEST(LbankPerpStream, ClassifiesEveryFrame) {
    EXPECT_EQ(parse_head(fixture("ws_book_snapshot.json")).action, Action::Book);
    EXPECT_EQ(parse_head(fixture("ws_book_delta.json")).action, Action::Book);
    EXPECT_EQ(parse_head(fixture("ws_trade.json")).action, Action::Trade);
    EXPECT_EQ(parse_head(fixture("ws_overview.json")).action, Action::Overview);
    EXPECT_EQ(parse_head(fixture("ws_resume_note.json")).action, Action::Note);
    const auto ack = parse_head(fixture("ws_ack.json"));
    EXPECT_EQ(ack.action, Action::Ack);
    EXPECT_EQ(ack.error_code, 0);
    const auto refusal = parse_head(fixture("ws_refusal.json"));
    EXPECT_EQ(refusal.action, Action::Ack);
    EXPECT_EQ(refusal.error_code, 5);
    EXPECT_EQ(parse_head("not json").action, Action::Unknown);
}

TEST(LbankPerpStream, ReportsATopicPastTheSocketCapAsARefusal) {
    EXPECT_EQ(parse_head(fixture("ws_topic_cap.json")).action, Action::Refused);
    EXPECT_EQ(parse_head(fixture("ws_speed_cap.json")).action, Action::Refused);
    EXPECT_EQ(parse_head(fixture("ws_resume_note.json")).action, Action::Note);
}

TEST(LbankPerpBook, SeedsFromTheSnapshotAndAppliesOnlyRowsAtItsOwnTick) {
    const auto btc = perp_base("BTCUSDT");
    const auto storage = std::make_unique<LevelBuffer>();
    book::Ladders ladders;

    const auto snapshot = parse_book(fixture("ws_book_snapshot.json"), *storage);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_TRUE(snapshot->snapshot);
    EXPECT_EQ(snapshot->index, "Exchange_BTCUSDT");
    ASSERT_EQ(snapshot->levels.size(), 6U);
    ASSERT_TRUE(apply_levels(*snapshot, "BTCUSDT", btc, ladders));
    auto touch = ladders.touch();
    ASSERT_TRUE(touch.has_value());
    EXPECT_EQ(touch->first.price.raw, 848'167);
    EXPECT_EQ(touch->first.qty.raw, 213'205);
    EXPECT_EQ(touch->second.price.raw, 848'168);
    EXPECT_EQ(touch->second.qty.raw, 83'386);

    const auto delta = parse_book(fixture("ws_book_delta.json"), *storage);
    ASSERT_TRUE(delta.has_value());
    EXPECT_FALSE(delta->snapshot);
    EXPECT_EQ(delta->bno, 218'065'390'699ULL);
    std::size_t coarse = 0;
    for (const auto& level : delta->levels) {
        coarse += level.instrument == "BTCUSDT" && !level.tick.empty() ? 1U : 0U;
    }
    EXPECT_GT(coarse, 0U);
    ASSERT_TRUE(apply_levels(*delta, "BTCUSDT", btc, ladders));
    EXPECT_EQ(ladders.bids().size(), 3U);
    EXPECT_EQ(ladders.asks().size(), 3U);
    touch = ladders.touch();
    ASSERT_TRUE(touch.has_value());
    EXPECT_EQ(touch->first.price.raw, 848'167);

    const auto book = ladders.snapshot(btc.id, delta->bno, k_ingest, k_ingest);
    ASSERT_TRUE(book.has_value());
    const auto top = top_of(*book);
    ASSERT_TRUE(top.has_value());
    EXPECT_EQ(top->bid.raw, 848'167);
    EXPECT_EQ(top->ask.raw, 848'168);
}

TEST(LbankPerpBook, RoundsAFloatTailOntoTheLot) {
    EXPECT_EQ(lots_of("112441.5999999999", e9("0.0001")), 1'124'416'000);
    EXPECT_EQ(lots_of("0", e9("0.0001")), 0);
    EXPECT_FALSE(ticks_of("84816.75", e9("0.1")).has_value());
}

TEST(LbankPerpBook, TakesEveryFrameOfABatchAndDropsAReplay) {
    const auto storage = std::make_unique<LevelBuffer>();
    std::uint64_t newest = 0;

    const auto first = parse_book(fixture("ws_book_batch_first.json"), *storage);
    ASSERT_TRUE(first.has_value());
    const auto batch = first->bno;
    EXPECT_EQ(first->index, "Exchange_QNTUSDT");
    EXPECT_TRUE(fresh_batch(batch, newest));

    const auto second = parse_book(fixture("ws_book_batch_second.json"), *storage);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->index, "Exchange_BTCUSDT");
    EXPECT_EQ(second->bno, batch);
    EXPECT_TRUE(fresh_batch(second->bno, newest));

    InstrumentBase ach{};
    ach.tick_size = e9("0.000001");
    ach.lot_size = e9("1");
    book::Ladders ladders;
    for (const auto& level : second->levels) {
        if (level.instrument == "ACHUSDT" && level.tick.empty()) {
            ASSERT_TRUE(apply_level(level, ach, ladders));
        }
    }
    const auto touch = ladders.touch();
    ASSERT_TRUE(touch.has_value());
    EXPECT_EQ(touch->first.price.raw, 6'283);
    EXPECT_EQ(touch->first.qty.raw, 55'676);
    EXPECT_EQ(touch->second.price.raw, 6'291);

    book::Ladders stale;
    stale.asks().upsert({.price = PriceTicks::from_raw(6'280), .qty = QtyLots::from_raw(1)});
    stale.asks().upsert({.price = PriceTicks::from_raw(6'283), .qty = QtyLots::from_raw(1)});
    stale.asks().upsert({.price = PriceTicks::from_raw(6'291), .qty = QtyLots::from_raw(1)});
    for (const auto& level : second->levels) {
        if (level.instrument == "ACHUSDT" && level.tick.empty() && !level.ask) {
            ASSERT_TRUE(apply_level(level, ach, stale));
        }
    }
    ASSERT_EQ(stale.asks().size(), 1U);
    EXPECT_EQ(stale.asks().levels().front().price.raw, 6'291);

    const auto replay = parse_book(fixture("ws_book_replay.json"), *storage);
    ASSERT_TRUE(replay.has_value());
    EXPECT_FALSE(fresh_batch(replay->bno, newest));
    EXPECT_EQ(newest, batch);
}

TEST(LbankPerpBook, ReseedsADriftedBookFromTheRestDepth) {
    const auto eth = perp_base("ETHUSDT");
    const auto storage = std::make_unique<RestDepthBuffer>();
    const auto rest = parse_market_order(fixture("rest_market_order.json"), *storage);
    ASSERT_TRUE(rest.has_value());
    ASSERT_EQ(rest->bids.size(), 5U);
    ASSERT_EQ(rest->asks.size(), 5U);

    book::Ladders ladders;
    ladders.bids().upsert({.price = PriceTicks::from_raw(267'949), .qty = QtyLots::from_raw(1'000)});
    ladders.asks().upsert({.price = PriceTicks::from_raw(267'950), .qty = QtyLots::from_raw(1'000)});
    EXPECT_FALSE(same_touch(ladders, *rest, eth));

    ASSERT_TRUE(seed_levels(*rest, eth, ladders));
    EXPECT_EQ(ladders.bids().size(), 5U);
    EXPECT_EQ(ladders.asks().size(), 5U);
    const auto touch = ladders.touch();
    ASSERT_TRUE(touch.has_value());
    EXPECT_EQ(touch->first.price.raw, 267'957);
    EXPECT_EQ(touch->first.qty.raw, 1'281'572);
    EXPECT_EQ(touch->second.price.raw, 267'958);
    EXPECT_TRUE(same_touch(ladders, *rest, eth));
}

TEST(LbankPerpTrades, StampsTheTradeWithTheVenueSecond) {
    const auto btc = perp_base("BTCUSDT");
    TradeBuffer storage;
    const auto rows = parse_trades(fixture("ws_trade.json"), storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1U);
    EXPECT_EQ(rows->front().trade_id, "1007934048397348");
    const auto trade = map_trade(rows->front(), btc, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->price.raw, 848'167);
    EXPECT_EQ(trade->qty.raw, 2);
    EXPECT_EQ(trade->aggressor, Side::Sell);
    EXPECT_EQ(trade->ts_event.ns, 1'790'964'229'000'000'000ULL);
    EXPECT_EQ(trade->ts_init, k_ingest);
}

TEST(LbankPerpOverview, CarriesFundingMarkAndTurnover) {
    const auto btc = perp_base("BTCUSDT");
    const auto storage = std::make_unique<OverviewBuffer>();
    const auto rows = parse_overview(fixture("ws_overview.json"), *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1U);
    const auto funding = map_funding(rows->front(), btc, 28'800, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate, e9("0.00003735"));
    EXPECT_EQ(funding->mark_price.raw, 848'426);
    EXPECT_EQ(funding->index_price.raw, 848'732);
    EXPECT_EQ(funding->interval_s, 28'800U);
    EXPECT_EQ(funding->accrual, FundingAccrual::Discrete);
    EXPECT_EQ(funding->ts_event.ns, 1'790'964'432'000'000'000ULL);
    EXPECT_EQ(funding->next_funding_time.ns, 1'790'985'600'000'000'000ULL);
    EXPECT_FALSE(map_funding(rows->front(), btc, 0, k_ingest).has_value());

    const auto stats = map_perp_stats(rows->front(), btc, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_quote, parse_turnover_e6("858830157.746951").value());
}

TEST(LbankSpotCatalogue, KeepsOnlyPairsWithATicker) {
    const auto storage = std::make_unique<SpotPairBuffer>();
    const auto rows = parse_spot_pairs(fixture("rest_accuracy.json"), *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 3U);
    const auto symbols = std::make_unique<SymbolBuffer>();
    const auto quoted = parse_spot_ticker_symbols(fixture("rest_ticker_24hr.json"), *symbols);
    ASSERT_TRUE(quoted.has_value());
    ASSERT_EQ(quoted->size(), 2U);
    std::size_t live = 0;
    for (const auto& row : *rows) {
        for (const auto symbol : *quoted) {
            live += row.symbol == symbol ? 1U : 0U;
        }
    }
    EXPECT_EQ(live, 2U);

    const auto btc = spot_base("btc_usdt");
    EXPECT_EQ(btc.tick_size, e9("0.01"));
    EXPECT_EQ(btc.lot_size, e9("0.00001"));
    EXPECT_EQ(btc.kind, ContractKind::Linear);
    EXPECT_EQ(btc.local_symbol.view(), "BTC/USDT");
    EXPECT_EQ(btc.exchange_symbol.view(), "btc_usdt");
}

TEST(LbankSpotStream, ClassifiesEveryFrame) {
    EXPECT_EQ(parse_spot_channel(fixture("spot_ws_depth.json")), SpotChannel::Depth);
    EXPECT_EQ(parse_spot_channel(fixture("spot_ws_trade.json")), SpotChannel::Trade);
    EXPECT_EQ(parse_spot_channel(fixture("spot_ws_tick.json")), SpotChannel::Tick);
    EXPECT_EQ(parse_spot_channel(fixture("spot_ws_pong.json")), SpotChannel::Pong);
    EXPECT_EQ(parse_spot_channel(fixture("spot_ws_error.json")), SpotChannel::Error);
}

TEST(LbankSpotStream, ReadsBeijingWallTime) {
    EXPECT_EQ(beijing_to_ms("2026-10-03T02:07:19.596"), 1'790'964'439'596ULL);
    EXPECT_EQ(beijing_to_ms("1970-01-01T08:00:00"), 0ULL);
    EXPECT_EQ(beijing_to_ms("2026-10-03 02:07:19"), 0ULL);
    EXPECT_EQ(beijing_to_ms(""), 0ULL);
}

TEST(LbankSpotBook, StampsTheDepthWithItsEngineMillisecond) {
    const auto btc = spot_base("btc_usdt");
    const auto storage = std::make_unique<SpotLevelBuffer>();
    const auto wire = parse_spot_depth(fixture("spot_ws_depth.json"), *storage);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->pair, "btc_usdt");
    EXPECT_EQ(wire->ts_ms, 1'790'964'439'467ULL);
    const auto book = map_spot_book(*wire, btc, k_ingest);
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->bid_count, 10U);
    EXPECT_EQ(book->ask_count, 10U);
    const auto top = top_of(*book);
    ASSERT_TRUE(top.has_value());
    EXPECT_EQ(top->bid.raw, 8'485'737);
    EXPECT_EQ(top->ask.raw, 8'485'738);
    EXPECT_EQ(top->ask_qty.raw, 1'477'643);
    EXPECT_EQ(top->ts_event.ns, 1'790'964'439'467'000'000ULL);
}

TEST(LbankSpotTrades, ReadsAnExponentVolume) {
    const auto btc = spot_base("btc_usdt");
    const auto wire = parse_spot_trade(fixture("spot_ws_trade.json"));
    ASSERT_TRUE(wire.has_value());
    const auto trade = map_spot_trade(*wire, btc, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->price.raw, 8'485'737);
    EXPECT_EQ(trade->qty.raw, 49);
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->ts_event.ns, 1'790'964'439'871'000'000ULL);
}

TEST(LbankSpotTicker, ReadsAnExponentTurnover) {
    const auto btc = spot_base("btc_usdt");
    const auto wire = parse_spot_tick(fixture("spot_ws_tick.json"));
    ASSERT_TRUE(wire.has_value());
    const auto stats = map_spot_stats(*wire, btc, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_quote, parse_turnover_e6("579529929.41").value());
    EXPECT_EQ(stats->volume_24h_base, e9("6757.9074"));
}

TEST(LbankSpotTicker, TheTableGivesEveryPairsVolumeInOneCall) {
    const auto btc = spot_base("btc_usdt");
    static SpotTickBuffer storage;
    const auto rows = parse_spot_tickers(fixture("rest_ticker_24hr.json"), storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 2U);
    EXPECT_EQ((*rows)[1].pair, "btc_usdt");
    const auto stats = map_spot_stats((*rows)[1], btc, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_quote, parse_turnover_e6("579545523.52").value());
    EXPECT_EQ(stats->volume_24h_base, e9("6758.1115"));
    EXPECT_EQ(stats->ts_event.ns, 1'790'964'172'175'000'000ULL);
    EXPECT_TRUE(stats->open_interest.is_undef());
}

TEST(LbankEndpoints, NamesTheDocumentedHosts) {
    EXPECT_EQ(ep::k_perp_ws_host, "lbkperpws.lbank.com");
    EXPECT_EQ(ep::k_spot_host, "api.lbank.info");
}

} // namespace
} // namespace raikiri::venues::lbank
