#include <raikiri/domain/asset_class.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/venues/ondoperps/map.hpp>
#include <raikiri/venues/ondoperps/wire.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>

namespace raikiri::venues::ondoperps {
namespace {

// the parsers hand out views into the body, so each fixture lives as long as the test binary
const std::string& fixture(std::string_view name) {
    static std::map<std::string, std::string, std::less<>> loaded;
    if (const auto found = loaded.find(name); found != loaded.end()) {
        return found->second;
    }
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return loaded.emplace(std::string{name}, buf.str()).first->second;
}

constexpr UnixNanos k_ingest{1'791'299'540'000'000'000ULL};

WireMarket row(std::string_view market) {
    static const auto body = fixture("rest_markets.json");
    static const auto storage = std::make_unique<MarketBuffer>();
    const auto rows = parse_markets(body, *storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& one : *rows) {
        if (one.market == market) {
            return one;
        }
    }
    ADD_FAILURE() << "no market " << market;
    return {};
}

InstrumentBase loaded(std::string_view market) {
    auto instrument = map_instrument(row(market), MarketType::Perp);
    EXPECT_TRUE(instrument.has_value()) << market;
    if (!instrument.has_value()) {
        return {};
    }
    classify(VenueId::OndoPerps, *instrument);
    return base_of(*instrument);
}

TEST(OndoCatalogue, EnabledPerpsWithTheVenuesClass) {
    EXPECT_FALSE(map_instrument(row("LIT-USD.P"), MarketType::Perp).has_value()) << "disabled";
    EXPECT_FALSE(map_instrument(row("EURUSD-USD.P"), MarketType::Perp).has_value()) << "disabled";
    const auto btc = loaded("BTC-USD.P");
    EXPECT_EQ(btc.local_symbol.view(), "BTC/USDC");
    EXPECT_EQ(btc.tick_size.raw, 1'000'000'000);
    EXPECT_EQ(btc.lot_size.raw, 100'000);
    EXPECT_EQ(btc.taker_fee.raw, 250'000);
    EXPECT_EQ(asset_key(btc), "crypto:BTC");
    EXPECT_EQ(asset_key(loaded("TSLA-USD.P")), "equity:TSLA");
    EXPECT_EQ(asset_key(loaded("SPY-USD.P")), "equity:SPY");
    EXPECT_EQ(asset_key(loaded("XAU-USD.P")), "commodity:XAU");
    EXPECT_EQ(asset_key(loaded("WTI-USD.P")), "commodity:CL");
    EXPECT_EQ(asset_key(loaded("US500-USD.P")), "index:SPX");
    EXPECT_EQ(asset_key(loaded("USDJPY-USD.P")), "fx:USDJPY");
    const auto perp = map_instrument(row("BTC-USD.P"), MarketType::Perp);
    ASSERT_TRUE(perp.has_value());
    EXPECT_EQ(std::get<PerpInstrument>(*perp).funding_cap.raw, 9'000'000);
}

TEST(OndoTimes, IsoWithNanosecondsAndTheServerClock) {
    EXPECT_EQ(iso_to_ns("2026-10-06T15:12:10.627606656Z"), 1'791'299'530'627'606'656ULL);
    EXPECT_EQ(iso_to_ns("2026-10-06T16:00:00Z"), 1'791'302'400'000'000'000ULL);
    EXPECT_EQ(iso_to_ns("2026-10-06T15:12:10.5Z"), 1'791'299'530'500'000'000ULL);
    EXPECT_FALSE(iso_to_ns("2026-10-06 15:12:10Z").has_value());
    EXPECT_EQ(server_time(fixture("rest_depth_time.json")), 1'791'299'417'222'801'009ULL);
}

TEST(OndoQuotes, TopOfBookAndTheFullDepthSnapshot) {
    BookBuffer storage;
    const auto top = fixture("ws_top.json");
    ASSERT_EQ(parse_frame(top), Frame::Top);
    const auto rows = parse_books(top, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1U);
    const auto btc = loaded("BTC-USD.P");
    const auto bbo = map_top((*rows)[0], btc, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 86'607);
    EXPECT_EQ(bbo->bid_qty.raw, 69);
    EXPECT_EQ(bbo->ask.raw, 86'609);
    EXPECT_EQ(bbo->ts_event.ns, 1'791'299'530'627'606'656ULL);

    const auto depth = fixture("ws_depth.json");
    ASSERT_EQ(parse_frame(depth), Frame::Depth);
    const auto books = parse_books(depth, storage);
    ASSERT_TRUE(books.has_value());
    const auto book = map_book((*books)[0], btc, k_ingest);
    ASSERT_TRUE(book.has_value());
    EXPECT_GT(book->bid_count, 1U);
    EXPECT_GT(book->asks[0].price.raw, book->bids[0].price.raw);
}

TEST(OndoTrades, TheAggressorsSide) {
    PrintBuffer storage;
    const auto rows = parse_trades(fixture("ws_trades.json"), storage);
    ASSERT_TRUE(rows.has_value());
    const auto trade = map_trade((*rows)[0], loaded("BTC-USD.P"), k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->price.raw, 86'610);
    EXPECT_EQ(trade->qty.raw, 1);
}

TEST(OndoFunding, TheRunningHourlyRateWithMarkAndTheHoursEnd) {
    const auto btc = loaded("BTC-USD.P");
    MarkBuffer marks;
    const auto rows = parse_marks(fixture("ws_marks.json"), marks);
    ASSERT_TRUE(rows.has_value());
    const WireMark* btc_mark = nullptr;
    for (const auto& one : *rows) {
        if (one.market == "BTC-USD.P") {
            btc_mark = &one;
        }
    }
    ASSERT_NE(btc_mark, nullptr);
    const auto mark = mark_ticks(*btc_mark, btc);
    ASSERT_TRUE(mark.has_value());
    EXPECT_EQ(mark->raw, 86'607);
    const auto index = oracle_ticks(*btc_mark, btc);
    ASSERT_TRUE(index.has_value());
    EXPECT_EQ(index->raw, 86'607);

    FundingBuffer storage;
    const auto funding_rows = parse_funding(fixture("ws_funding.json"), storage);
    ASSERT_TRUE(funding_rows.has_value());
    const auto funding = map_funding((*funding_rows)[0], btc, *mark, *index, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 12'500);
    EXPECT_EQ(funding->mark_price.raw, 86'607);
    EXPECT_EQ(funding->index_price.raw, 86'607);
    EXPECT_EQ(funding->next_funding_time.ns, 1'791'302'400'000'000'000ULL);
    EXPECT_EQ(funding->interval_s, 3600U);
    EXPECT_EQ(funding->ts_event.ns, 1'791'299'531'639'613'941ULL);
}

TEST(OndoTicker, VolumeAndOpenInterest) {
    StatBuffer interest;
    StatBuffer volumes;
    const auto oi = parse_open_interest(fixture("rest_open_interest.json"), interest);
    const auto vol = parse_volumes(fixture("rest_volume.json"), volumes);
    ASSERT_TRUE(oi.has_value());
    ASSERT_TRUE(vol.has_value());
    Stats stats{};
    for (const auto& one : *oi) {
        if (one.market == "BTC-USD.P") {
            stats.open_interest = parse_amount_e9(one.first).value_or(FixedE9{});
        }
    }
    for (const auto& one : *vol) {
        if (one.market == "BTC-USD.P") {
            stats.volume_quote = parse_turnover_e6(one.second).value_or(FixedE6{});
        }
    }
    const auto ticker = map_ticker(stats, loaded("BTC-USD.P"), k_ingest);
    EXPECT_EQ(ticker.open_interest.raw, 45'977'100'000);
    EXPECT_EQ(ticker.volume_24h_quote.raw, 14'752'161'120'000);
}

TEST(OndoControl, AcksErrorsAndCaps) {
    EXPECT_EQ(parse_frame(fixture("ws_subscribed.json")), Frame::Subscribed);
    EXPECT_EQ(parse_frame(fixture("ws_error.json")), Frame::Error);
    EXPECT_EQ(parse_frame(fixture("ws_pong.json")), Frame::Pong);
    EXPECT_FALSE(exec::caps_of(VenueId::OndoPerps).cancel_by_client_order_id);
    EXPECT_EQ(to_string(VenueId::OndoPerps), "ondoperps");
}

} // namespace
} // namespace raikiri::venues::ondoperps
