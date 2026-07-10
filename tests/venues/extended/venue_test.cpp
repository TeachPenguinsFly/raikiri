#include <raikiri/domain/asset_class.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/venues/extended/book.hpp>
#include <raikiri/venues/extended/catalogue.hpp>
#include <raikiri/venues/extended/endpoints.hpp>
#include <raikiri/venues/extended/map.hpp>
#include <raikiri/venues/extended/market_data.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace raikiri::venues::extended {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

constexpr UnixNanos k_ingest{1'790'315'100'000'000'000ULL};

FixedE9 e9(std::string_view text) {
    return parse_decimal_e9(text).value_or(FixedE9{});
}

WireMarket row(std::string_view name) {
    static const auto body = fixture("rest_markets.json");
    static const auto storage = std::make_unique<MarketBuffer>();
    const auto rows = parse_markets(body, *storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& one : *rows) {
        if (one.name == name) {
            return one;
        }
    }
    ADD_FAILURE() << "no market " << name;
    return {};
}

InstrumentBase base_for(std::string_view name) {
    const auto instrument = map_instrument(row(name), MarketType::Perp);
    EXPECT_TRUE(instrument.has_value()) << name;
    auto base = base_of(*instrument);
    base.id = InstrumentId::make(VenueId::Extended, MarketType::Perp, SymbolId{7});
    return base;
}

TEST(ExtendedCatalogue, ReadsTheAssetClassOffTheSubCategory) {
    const std::string body = R"({"status":"OK","data":[)"
        R"({"name":"TSLA_24_5-USD","category":"RWA","subCategory":"Equity","type":"PERPETUAL","status":"ACTIVE","isRfq":false,)"
        R"("marketStats":{},"tradingConfig":{"minOrderSize":"0.1","minOrderSizeChange":"0.1","minPriceChange":"0.01"}},)"
        R"({"name":"XAU-USD","category":"RWA","subCategory":"Commodity","type":"PERPETUAL","status":"ACTIVE","isRfq":false,)"
        R"("marketStats":{},"tradingConfig":{"minOrderSize":"0.01","minOrderSizeChange":"0.01","minPriceChange":"0.1"}},)"
        R"({"name":"SPX-USD","category":"Crypto","subCategory":"Meme","type":"PERPETUAL","status":"ACTIVE","isRfq":false,)"
        R"("marketStats":{},"tradingConfig":{"minOrderSize":"1","minOrderSizeChange":"1","minPriceChange":"0.0001"}},)"
        R"({"name":"NEW-USD","category":"RWA","subCategory":"Bond","type":"PERPETUAL","status":"ACTIVE","isRfq":false,)"
        R"("marketStats":{},"tradingConfig":{"minOrderSize":"1","minOrderSizeChange":"1","minPriceChange":"0.01"}}]})";
    const auto storage = std::make_unique<MarketBuffer>();
    const auto rows = parse_markets(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);
    const auto classified = [&](std::size_t i) {
        const auto instrument = map_instrument((*rows)[i], MarketType::Perp);
        EXPECT_TRUE(instrument.has_value());
        auto base = base_of(instrument.value_or(PerpInstrument{}));
        const auto stated = base.asset_class;
        classify(VenueId::Extended, base);
        return std::pair{stated, base};
    };
    const auto [tsla_stated, tsla] = classified(0);
    EXPECT_EQ(tsla_stated, AssetClass::Equity);
    EXPECT_EQ(tsla.underlying.view(), "TSLA");
    const auto [xau_stated, xau] = classified(1);
    EXPECT_EQ(xau_stated, AssetClass::Commodity);
    EXPECT_EQ(xau.underlying.view(), "XAU");
    const auto [spx_stated, spx] = classified(2);
    EXPECT_EQ(spx_stated, AssetClass::Crypto);
    EXPECT_EQ(spx.asset_class, AssetClass::Crypto);
    EXPECT_EQ(spx.underlying.view(), "SPX");
    const auto [new_stated, unknown] = classified(3);
    EXPECT_EQ(new_stated, AssetClass::Unknown);
    EXPECT_EQ(unknown.asset_class, AssetClass::Unknown);
    EXPECT_TRUE(unknown.underlying.empty());
}

TEST(ExtendedCatalogue, ParsesEveryRowAndReadsTheSettlementTimeAsATimestamp) {
    const auto body = fixture("rest_markets.json");
    const auto storage = std::make_unique<MarketBuffer>();
    const auto rows = parse_markets(body, *storage);
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 10U);
    const auto btc = row("BTC-USD");
    EXPECT_EQ(btc.status, "ACTIVE");
    EXPECT_FALSE(btc.rfq);
    EXPECT_EQ(btc.tick, "1");
    EXPECT_EQ(btc.lot, "0.00001");
    EXPECT_EQ(btc.next_funding_ms, 1'790'316'000'000ULL);
    EXPECT_TRUE(row("DASH-USD").rfq);
}

TEST(ExtendedCatalogue, LoadsActivePerpsOnlyAndKeepsTheScaledNamesApart) {
    const auto btc = map_instrument(row("BTC-USD"), MarketType::Perp);
    ASSERT_TRUE(btc.has_value());
    const auto& perp = std::get<PerpInstrument>(*btc);
    EXPECT_EQ(perp.base.exchange_symbol.view(), "BTC-USD");
    EXPECT_EQ(perp.base.local_symbol.view(), "BTC/USDC");
    EXPECT_EQ(perp.base.tick_size, e9("1"));
    EXPECT_EQ(perp.base.lot_size, e9("0.00001"));
    EXPECT_EQ(perp.base.kind, ContractKind::Linear);
    EXPECT_EQ(perp.funding_interval_s, 3600U);
    EXPECT_TRUE(perp.funding_cap.is_undef());

    EXPECT_EQ(base_for("kFLOKI-USD").local_symbol.view(), "KFLOKI/USDC");
    EXPECT_EQ(base_for("1000PEPE-USD").local_symbol.view(), "1000PEPE/USDC");
    EXPECT_EQ(base_for("TSLA_24_5-USD").local_symbol.view(), "TSLA_24_5/USDC");

    const auto delisted = row("NOW_24_5-USD");
    EXPECT_EQ(delisted.status, "DELISTED");
    EXPECT_FALSE(map_instrument(delisted, MarketType::Perp).has_value());
    EXPECT_FALSE(map_instrument(row("GILD-USD"), MarketType::Perp).has_value());
    EXPECT_FALSE(map_instrument(row("BTCSPOT-USD"), MarketType::Perp).has_value());
    EXPECT_FALSE(map_instrument(row("BTC-USD"), MarketType::Spot).has_value());
}

TEST(ExtendedBook, SnapshotReadsQAndDeltaReadsTheAbsoluteC) {
    const auto base = base_for("ADA-USD");
    Book book{base.id, base.tick_size, base.lot_size};
    const auto storage = std::make_unique<LevelBuffer>();

    const auto snapshot = fixture("ws_book_snapshot.json");
    const auto snap = parse_book(snapshot, *storage);
    ASSERT_TRUE(snap.has_value());
    EXPECT_TRUE(snap->snapshot);
    EXPECT_EQ(snap->seq, 1420U);
    EXPECT_EQ(book.apply(*snap), BookResult::Resynced);
    ASSERT_EQ(book.bids().size(), 18U);
    EXPECT_EQ(book.bids()[0].price.raw, 2473);
    EXPECT_EQ(book.bids()[0].qty.raw, 181617);

    const auto frame = fixture("ws_book_delta.json");
    const auto delta = parse_book(frame, *storage);
    ASSERT_TRUE(delta.has_value());
    EXPECT_FALSE(delta->snapshot);
    EXPECT_EQ(book.apply(*delta), BookResult::Applied);
    const auto top = book.top(k_ingest, k_ingest);
    ASSERT_TRUE(top.has_value());
    // 0.2474 moved from the ask (c "0") to the bid (c "165357").
    EXPECT_EQ(top->bid.raw, 2474);
    EXPECT_EQ(top->bid_qty.raw, 165357);
    EXPECT_EQ(top->ask.raw, 2475);
    EXPECT_EQ(top->ask_qty.raw, 44384);
    EXPECT_EQ(book.bids()[1].qty.raw, 176492);
}

TEST(ExtendedBook, ADeltaWithoutASnapshotIsIgnored) {
    const auto base = base_for("ADA-USD");
    Book book{base.id, base.tick_size, base.lot_size};
    const auto storage = std::make_unique<LevelBuffer>();
    const auto frame = fixture("ws_book_delta.json");
    const auto delta = parse_book(frame, *storage);
    ASSERT_TRUE(delta.has_value());
    EXPECT_EQ(book.apply(*delta), BookResult::Ignored);
    EXPECT_FALSE(book.top(k_ingest, k_ingest).has_value());
}

TEST(ExtendedBook, DelistedSnapshotsParseAndNameAMarketNotInTheCatalogue) {
    const auto storage = std::make_unique<LevelBuffer>();
    const auto frame = fixture("ws_book_delisted.json");
    const auto wire = parse_book(frame, *storage);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->market, "MOODENG-USD");
    EXPECT_EQ(wire->ts_ms, 0U);
}

TEST(ExtendedBook, DepthOneFrameIsTheQuote) {
    const auto base = base_for("ADA-USD");
    const auto storage = std::make_unique<LevelBuffer>();
    const auto frame = fixture("ws_top.json");
    const auto wire = parse_book(frame, *storage);
    ASSERT_TRUE(wire.has_value());
    const auto top = map_top(*wire, base.id, base.tick_size, base.lot_size, k_ingest);
    ASSERT_TRUE(top.has_value());
    EXPECT_EQ(top->bid.raw, 2473);
    EXPECT_EQ(top->bid_qty.raw, 181517);
    EXPECT_EQ(top->ask.raw, 2474);
    EXPECT_EQ(top->ts_event.ns, 1'790'315'091'769'000'000ULL);
}

TEST(ExtendedTrades, BacklogAndLivePrintsCarryTheTakerSide) {
    PrintBuffer storage;
    const auto backlog = fixture("ws_trades_backlog.json");
    const auto replay = parse_trades(backlog, storage);
    ASSERT_TRUE(replay.has_value());
    EXPECT_EQ(replay->prints.size(), 50U);
    EXPECT_EQ(replay->seq, 1U);

    const auto frame = fixture("ws_trades.json");
    const auto live = parse_trades(frame, storage);
    ASSERT_TRUE(live.has_value());
    ASSERT_EQ(live->prints.size(), 2U);
    const auto& print = live->prints[0];
    EXPECT_EQ(print.id, 2'103'360'060'464'828'416ULL);
    const auto trade = map_trade(print, base_for("BTC-USD"), k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->price.raw, 84207);
    EXPECT_EQ(trade->qty.raw, 5034);
    EXPECT_EQ(trade->ts_event.ns, 1'790'315'091'612'000'000ULL);
}

TEST(ExtendedFunding, RateAndMarkArriveApartAndPublishTogether) {
    const auto btc = base_for("BTC-USD");
    FundingMemo memo{};
    const auto frame = fixture("ws_funding.json");
    const auto funding = parse_funding(frame);
    ASSERT_TRUE(funding.has_value());
    ASSERT_TRUE(note_rate(*funding, memo));
    auto update = merged(btc, memo, UnixNanos{funding->ts_ms * 1'000'000ULL}, k_ingest);
    EXPECT_EQ(update.rate, e9("0.000009"));
    EXPECT_TRUE(update.mark_price.is_undef());
    EXPECT_TRUE(update.predicted_rate.is_undef());
    EXPECT_EQ(update.interval_s, 3600U);

    note_market(row("BTC-USD"), btc, memo);
    EXPECT_EQ(memo.rate, e9("0.000009")) << "the REST row must not overwrite the stream's rate";
    update = merged(btc, memo, k_ingest, k_ingest);
    EXPECT_EQ(update.next_funding_time.ns, 1'790'316'000'000'000'000ULL);
    EXPECT_FALSE(update.mark_price.is_undef());
    // A settlement already past is not published.
    EXPECT_TRUE(
        merged(btc, memo, UnixNanos{1'790'316'000'000'000'001ULL}, k_ingest).next_funding_time.ns ==
        0);
}

TEST(ExtendedFunding, MarkIsTruncatedOntoTheTickAndTakesTheFrameTime) {
    const auto dash = base_for("DASH-USD");
    const auto frame = fixture("ws_mark.json");
    const auto mark = parse_mark(frame);
    ASSERT_TRUE(mark.has_value());
    EXPECT_EQ(mark->ts_ms, 1'790'315'091'741ULL);
    FundingMemo memo{};
    ASSERT_TRUE(note_mark(*mark, dash, memo));
    EXPECT_EQ(memo.mark.raw, 62663); // 62.66350392 on a 0.001 tick
    EXPECT_TRUE(memo.rate.is_undef());
}

TEST(ExtendedFunding, GhostMarketsParseAndResolveToNothing) {
    const auto frame = fixture("ws_funding_ghost.json");
    const auto ghost = parse_funding(frame);
    ASSERT_TRUE(ghost.has_value());
    EXPECT_EQ(ghost->market, "MOG-USD");
}

TEST(ExtendedCaps, PerpOnlyAndNothingAnOrderPathWouldNeed) {
    const auto& caps = exec::caps_of(VenueId::Extended);
    EXPECT_EQ(caps.venue, VenueId::Extended);
    EXPECT_TRUE(caps.markets.perp);
    EXPECT_FALSE(caps.markets.spot);
    EXPECT_EQ(caps.max_client_order_id_len, 0U);
    EXPECT_TRUE(caps.ticker_stats);
    EXPECT_EQ(to_string(VenueId::Extended), "extended");
    EXPECT_EQ(ep::target(ep::Stream::Top), "/stream.extended.exchange/v1/orderbooks?depth=1");
}

} // namespace
} // namespace raikiri::venues::extended
