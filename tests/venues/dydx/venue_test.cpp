#include <raikiri/venues/dydx/book.hpp>
#include <raikiri/venues/dydx/endpoints.hpp>
#include <raikiri/venues/dydx/map.hpp>
#include <raikiri/venues/dydx/wire.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>

namespace raikiri::venues::dydx {
namespace {

// Cached for the process: parsed frames are views into these bytes.
const std::string& fixture(std::string_view name) {
    static std::map<std::string, std::string, std::less<>> loaded;
    if (const auto hit = loaded.find(name); hit != loaded.end()) {
        return hit->second;
    }
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return loaded.emplace(std::string{name}, buf.str()).first->second;
}

// 2026-09-25T06:00:00Z minus ten minutes.
constexpr UnixNanos k_ingest{1'790'315'400'000'000'000ULL};

FixedE9 e9(std::string_view text) {
    const auto value = parse_decimal_e9(text);
    EXPECT_TRUE(value.has_value()) << text;
    return value.value_or(FixedE9{});
}

WireMarket row(std::string_view ticker) {
    static const auto& body = fixture("rest_perpetual_markets.json");
    static MarketBuffer storage;
    const auto rows = parse_perpetual_markets(body, storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& market : *rows) {
        if (market.ticker == ticker) {
            return market;
        }
    }
    ADD_FAILURE() << "no market " << ticker;
    return {};
}

InstrumentBase base_for(std::string_view ticker) {
    const auto instrument = map_instrument(row(ticker));
    EXPECT_TRUE(instrument.has_value());
    auto base = base_of(*instrument);
    base.id = InstrumentId::make(VenueId::Dydx, MarketType::Perp, SymbolId{1});
    return base;
}

std::unique_ptr<FrameBuffer> buffer() {
    return std::make_unique<FrameBuffer>();
}

TEST(DydxCatalogue, EveryRowParsesIncludingTheLongestTicker) {
    MarketBuffer storage;
    const auto& body = fixture("rest_perpetual_markets.json");
    const auto rows = parse_perpetual_markets(body, storage);
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 7U);
}

TEST(DydxCatalogue, APerpIsUsdcMarginedAndSettlesHourly) {
    const auto instrument = map_instrument(row("BTC-USD"));
    ASSERT_TRUE(instrument.has_value());
    const auto& perp = std::get<PerpInstrument>(*instrument);
    EXPECT_EQ(perp.base.exchange_symbol.view(), "BTC-USD");
    EXPECT_EQ(perp.base.local_symbol.view(), "BTC/USDC");
    EXPECT_EQ(perp.base.tick_size, e9("1"));
    EXPECT_EQ(perp.base.lot_size, e9("0.0001"));
    EXPECT_EQ(perp.base.kind, ContractKind::Linear);
    EXPECT_EQ(perp.funding_interval_s, 3600U);
}

TEST(DydxCatalogue, TheQuantumFieldsAndTheStatedGridAgree) {
    for (const auto* ticker : {"BTC-USD", "ETH-USD", "TRX-USD", "APE-USD"}) {
        const auto market = row(ticker);
        EXPECT_EQ(quantum_tick(market), e9(market.tick_size)) << ticker;
        EXPECT_EQ(quantum_lot(market), e9(market.step_size)) << ticker;
    }
}

TEST(DydxCatalogue, PepesTickIsRefusedNotRounded) {
    const auto pepe = row("PEPE-USD");
    EXPECT_EQ(pepe.tick_size, "0.0000000001");
    EXPECT_FALSE(quantum_tick(pepe).has_value());
    const auto mapped = map_instrument(pepe);
    ASSERT_FALSE(mapped.has_value());
    EXPECT_EQ(mapped.error(), Refusal::TooFine);
}

TEST(DydxCatalogue, SettledMarketsAreNotLoaded) {
    EXPECT_EQ(map_instrument(row("MATIC-USD")).error(), Refusal::Inactive);
    EXPECT_EQ(map_instrument(row("CIG,RAYDIUM,EXRQAUXS967FDKYMNXO4EQZEQIHEUBSTBCXPXXWPUMP-USD")).error(),
              Refusal::Inactive);
}

TEST(DydxFunding, ATwentyDecimalRateAndAnOffGridOracleBothArrive) {
    const auto btc = row("BTC-USD");
    EXPECT_EQ(btc.next_funding_rate, "-0.00000052594339622642");
    EXPECT_FALSE(parse_decimal_e9(btc.next_funding_rate).has_value());
    const auto funding = map_funding(btc, base_for("BTC-USD"), k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate, FixedE9::from_raw(-525));
    EXPECT_TRUE(funding->predicted_rate.is_undef());
    EXPECT_EQ(btc.oracle_price, "84235.67822");
    EXPECT_EQ(funding->mark_price, PriceTicks::from_raw(84235));
    EXPECT_EQ(funding->index_price, funding->mark_price);
    EXPECT_EQ(funding->accrual, FundingAccrual::Discrete);
    EXPECT_EQ(funding->interval_s, 3600U);
    EXPECT_EQ(funding->next_funding_time.ns, 1'790'316'000'000'000'000ULL);
}

TEST(DydxFunding, AnOraclePastNineDecimalsIsTruncatedOntoTheTick) {
    const auto trx = row("TRX-USD");
    EXPECT_EQ(trx.oracle_price, "0.3387554574");
    EXPECT_EQ(trx.next_funding_rate, "0.00004455896226415094");
    const auto funding = map_funding(trx, base_for("TRX-USD"), k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate, FixedE9::from_raw(44558));
    EXPECT_EQ(funding->mark_price, PriceTicks::from_raw(33875)); // tick 0.00001
}

TEST(DydxFunding, NoRateIsNoUpdate) {
    auto btc = row("BTC-USD");
    btc.next_funding_rate = {};
    EXPECT_FALSE(map_funding(btc, base_for("BTC-USD"), k_ingest).has_value());
}

TEST(DydxFunding, TheNextSettlementIsTheNextHour) {
    EXPECT_EQ(next_hour(UnixNanos{3'600'000'000'000ULL}).ns, 7'200'000'000'000ULL);
    EXPECT_EQ(next_hour(UnixNanos{3'599'999'999'999ULL}).ns, 3'600'000'000'000ULL);
}

TEST(DydxFunding, TurnoverIsQuoteOnly) {
    const auto stats = map_ticker_stats(row("BTC-USD"), base_for("BTC-USD"), k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_quote, *parse_turnover_e6("7721124.7643"));
    EXPECT_TRUE(stats->volume_24h_base.is_undef());
    EXPECT_EQ(stats->open_interest, *parse_decimal_e9("191.6459"));
}

TEST(DydxBook, TheSnapshotSeedsABandedBookAndTheBatchApplies) {
    auto storage = buffer();
    const auto base = base_for("BTC-USD");
    Book book{base.id, base.tick_size, base.lot_size};

    const auto snapshot = parse_frame(fixture("ws_book_snapshot.json"), *storage);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->type, FrameType::Subscribed);
    EXPECT_EQ(snapshot->channel, Channel::Book);
    EXPECT_EQ(snapshot->id, "BTC-USD");
    EXPECT_EQ(snapshot->bids.size(), k_snapshot_depth);
    EXPECT_EQ(book.reset(snapshot->bids, snapshot->asks), BookResult::Resynced);

    const auto batch = parse_frame(fixture("ws_book_batch.json"), *storage);
    ASSERT_TRUE(batch.has_value());
    EXPECT_EQ(batch->type, FrameType::Data);
    EXPECT_FALSE(batch->overflow);
    EXPECT_GT(batch->bids.size() + batch->asks.size(), 0U);
    EXPECT_EQ(book.apply(batch->bids, batch->asks), BookResult::Applied);
    const auto top = book.top(k_ingest, k_ingest);
    ASSERT_TRUE(top.has_value());
    EXPECT_LT(top->bid.raw, top->ask.raw);
    EXPECT_GT(book.outside_band(), 0U);
}

TEST(DydxBook, AShallowSnapshotIsTheWholeSideAndIsNotBanded) {
    auto storage = buffer();
    const auto base = base_for("APE-USD");
    Book book{base.id, base.tick_size, base.lot_size};
    const auto snapshot = parse_frame(fixture("ws_book_snapshot_shallow.json"), *storage);
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_LT(snapshot->bids.size(), k_snapshot_depth);
    ASSERT_EQ(book.reset(snapshot->bids, snapshot->asks), BookResult::Resynced);
    const std::array<WireLevel, 1> far{{{.price = "0.0001", .size = "10"}}};
    EXPECT_EQ(book.apply(far, {}), BookResult::Applied);
    EXPECT_EQ(book.outside_band(), 0U);
}

TEST(DydxBook, ACrossedBookPublishesNothingAndAsksAgainOncePerCooldown) {
    const auto base = base_for("BTC-USD");
    Book book{base.id, base.tick_size, base.lot_size};
    const std::array<WireLevel, 1> bid{{{.price = "84000", .size = "1"}}};
    const std::array<WireLevel, 1> ask{{{.price = "84100", .size = "1"}}};
    ASSERT_EQ(book.reset(bid, ask), BookResult::Resynced);
    EXPECT_FALSE(book.take_resync(k_ingest.ns));

    const std::array<WireLevel, 1> through{{{.price = "84100", .size = "2"}}};
    EXPECT_EQ(book.apply(through, {}), BookResult::Crossed);
    EXPECT_FALSE(book.top(k_ingest, k_ingest).has_value());
    EXPECT_FALSE(book.snapshot(k_ingest, k_ingest).has_value());
    EXPECT_TRUE(book.take_resync(k_ingest.ns));
    EXPECT_EQ(book.apply(through, {}), BookResult::Crossed);
    EXPECT_FALSE(book.take_resync(k_ingest.ns + k_resync_cooldown_ns - 1));
    EXPECT_TRUE(book.take_resync(k_ingest.ns + k_resync_cooldown_ns));
    EXPECT_EQ(book.crossings(), 1U);
}

TEST(DydxBook, AnOffGridLevelDropsTheBook) {
    const auto base = base_for("BTC-USD");
    Book book{base.id, base.tick_size, base.lot_size};
    const std::array<WireLevel, 1> bid{{{.price = "84000", .size = "1"}}};
    const std::array<WireLevel, 1> ask{{{.price = "84100", .size = "1"}}};
    ASSERT_EQ(book.reset(bid, ask), BookResult::Resynced);
    const std::array<WireLevel, 1> bad{{{.price = "84000.5", .size = "1"}}};
    EXPECT_EQ(book.apply(bad, {}), BookResult::Lost);
    EXPECT_FALSE(book.synced());
    EXPECT_TRUE(book.take_resync(k_ingest.ns));
}

TEST(DydxBook, AnUnbatchedUpdateCarriesPairs) {
    auto storage = buffer();
    const auto frame = parse_frame(fixture("ws_book_update.json"), *storage);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->id, "ETH-USD");
    ASSERT_EQ(frame->asks.size(), 1U);
    EXPECT_EQ(frame->asks[0].price, "2686.5");
    EXPECT_EQ(frame->asks[0].size, "87.055");
    EXPECT_TRUE(frame->bids.empty());
}

TEST(DydxBook, AMemeCoinSizePastFixedE9IsReadExactly) {
    EXPECT_FALSE(parse_decimal_e9("13433012900").has_value());
    EXPECT_EQ(exact_steps("13433012900", e9("100")), 134330129);
    EXPECT_FALSE(exact_steps("13433012950", e9("100")).has_value());
    EXPECT_EQ(exact_steps("0.0001", e9("0.0001")), 1);
    EXPECT_FALSE(exact_steps("0.0000000001", e9("0.000000001")).has_value());
}

TEST(DydxTrades, TheBacklogIsNotRead) {
    auto storage = buffer();
    const auto frame = parse_frame(fixture("ws_trades_subscribed.json"), *storage);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->type, FrameType::Subscribed);
    EXPECT_EQ(frame->channel, Channel::Trades);
    EXPECT_TRUE(frame->prints.empty());
}

TEST(DydxTrades, ALivePrintCarriesTheTakerSideAndItsTime) {
    auto storage = buffer();
    const auto frame = parse_frame(fixture("ws_trades_update.json"), *storage);
    ASSERT_TRUE(frame.has_value());
    ASSERT_EQ(frame->prints.size(), 3U);
    const auto trade = map_trade(frame->prints[2], base_for("BTC-USD"), k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->price, PriceTicks::from_raw(84325));
    EXPECT_EQ(trade->qty, QtyLots::from_raw(46));
    EXPECT_EQ(trade->ts_event, parse_iso8601("2026-09-25T05:48:08.596Z").value());
    EXPECT_EQ(trade->ts_event.ns, 1'790'315'288'596'000'000ULL);
}

TEST(DydxTrades, ABatchedTapeReadsLikeAnUnbatchedOne) {
    auto storage = buffer();
    const auto frame = parse_frame(fixture("ws_trades_batch.json"), *storage);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->id, "ETH-USD");
    ASSERT_EQ(frame->prints.size(), 1U);
    EXPECT_EQ(frame->prints[0].side, "BUY");
}

TEST(DydxStream, ALimitRefusalNamesItsChannelOnlyInItsText) {
    auto storage = buffer();
    const auto frame = parse_frame(fixture("ws_error_limit.json"), *storage);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->type, FrameType::Error);
    EXPECT_EQ(frame->channel, Channel::Other);
    EXPECT_TRUE(frame->id.empty());
    EXPECT_EQ(refused_channel(frame->message), Channel::Book);
}

TEST(DydxStream, ControlFramesAreRecognised) {
    auto storage = buffer();
    EXPECT_EQ(parse_frame(fixture("ws_connected.json"), *storage)->type, FrameType::Connected);
    EXPECT_EQ(parse_frame(fixture("ws_unsubscribed.json"), *storage)->type, FrameType::Unsubscribed);
}

TEST(DydxStream, BooksAreBatchedAndTradesAreNot) {
    Protocol protocol;
    EXPECT_EQ(protocol.subscribe(Channel::Book, "BTC-USD"),
              R"({"type":"subscribe","channel":"v4_orderbook","id":"BTC-USD","batched":true})");
    EXPECT_EQ(protocol.subscribe(Channel::Trades, "BTC-USD"),
              R"({"type":"subscribe","channel":"v4_trades","id":"BTC-USD"})");
    EXPECT_EQ(protocol.unsubscribe(Channel::Book, "BTC-USD"),
              R"({"type":"unsubscribe","channel":"v4_orderbook","id":"BTC-USD"})");
    EXPECT_TRUE(protocol.subscribe(Channel::Other, "BTC-USD").empty());
}

} // namespace
} // namespace raikiri::venues::dydx
