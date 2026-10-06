#include <raikiri/domain/asset_class.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/venues/polymarketperps/map.hpp>
#include <raikiri/venues/polymarketperps/wire.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>

namespace raikiri::venues::polymarketperps {
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

WireInstrument row(std::uint64_t iid) {
    static const auto body = fixture("rest_instruments.json");
    static const auto storage = std::make_unique<InstrumentBuffer>();
    const auto rows = parse_instruments(body, *storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& one : *rows) {
        if (one.iid == iid) {
            return one;
        }
    }
    ADD_FAILURE() << "no instrument " << iid;
    return {};
}

InstrumentBase loaded(std::uint64_t iid) {
    auto instrument = map_instrument(row(iid), MarketType::Perp);
    EXPECT_TRUE(instrument.has_value()) << iid;
    if (!instrument.has_value()) {
        return {};
    }
    classify(VenueId::PolymarketPerps, *instrument);
    return base_of(*instrument);
}

TEST(PolymarketCatalogue, PerpsWithTheVenuesCategory) {
    const auto btc = loaded(6);
    EXPECT_EQ(btc.local_symbol.view(), "BTC/USDC");
    EXPECT_EQ(btc.exchange_symbol.view(), "BTC-USD");
    EXPECT_EQ(btc.tick_size.raw, 100'000'000);
    EXPECT_EQ(btc.lot_size.raw, 10'000);
    EXPECT_EQ(btc.min_notional.raw, 10'000'000'000);
    EXPECT_EQ(asset_key(btc), "crypto:BTC");
    EXPECT_EQ(asset_key(loaded(1)), "index:SPX");
    EXPECT_EQ(asset_key(loaded(2)), "commodity:XAU");
    EXPECT_EQ(asset_key(loaded(3)), "commodity:CL");
    EXPECT_EQ(asset_key(loaded(21)), "equity:TSLA");
    auto closing = row(6);
    closing.close_only = true;
    EXPECT_FALSE(map_instrument(closing, MarketType::Perp).has_value());
}

TEST(PolymarketQuotes, BboFramesAndTheRestSeed) {
    const auto frame = fixture("ws_bbo.json");
    ASSERT_EQ(parse_frame(frame), Frame::Bbo);
    const auto wire = parse_bbo(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->iid, 6U);
    EXPECT_EQ(wire->sq, 64'349'747'039U);
    const auto bbo = map_bbo(*wire, loaded(6), k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 865'710);
    EXPECT_EQ(bbo->bid_qty.raw, 5'774);
    EXPECT_EQ(bbo->ts_event.ns, 1'791'299'532'434'000'000ULL);

    BboBuffer storage;
    const auto rows = parse_bbo_rows(fixture("rest_bbo.json"), storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 5U);
    for (const auto& one : *rows) {
        EXPECT_TRUE(map_bbo(one, loaded(one.iid), k_ingest).has_value()) << one.iid;
    }
}

TEST(PolymarketQuotes, TheSharedCounterOrdersButDoesNotFindGaps) {
    LastSeq seq;
    EXPECT_TRUE(seq.admit(10));
    EXPECT_TRUE(seq.admit(25));
    EXPECT_FALSE(seq.admit(25));
    EXPECT_FALSE(seq.admit(11));
}

TEST(PolymarketBook, EveryBookFrameIsTheWholeLadder) {
    const auto frame = fixture("ws_book.json");
    ASSERT_EQ(parse_frame(frame), Frame::Book);
    const auto storage = std::make_unique<LevelBuffer>();
    const auto wire = parse_book(frame, *storage);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->iid, 21U);
    const auto book = map_book(*wire, loaded(21), k_ingest);
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->asks[0].price.raw, 38'077);
    EXPECT_EQ(book->venue_seq, 64'349'742'572U);
    const auto top = top_of(*book);
    ASSERT_TRUE(top.has_value());
    EXPECT_LT(top->bid.raw, top->ask.raw);
}

TEST(PolymarketTrades, LongIsABuyingTaker) {
    PrintBuffer storage;
    const auto rows = parse_trades(fixture("ws_trades.json"), storage);
    ASSERT_TRUE(rows.has_value());
    auto print = (*rows)[0];
    const auto trade = map_trade(print, loaded(6), k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->price.raw, 865'790);
    EXPECT_EQ(trade->qty.raw, 24);
    EXPECT_EQ(trade->ts_event.ns, 1'791'299'539'342'000'000ULL);
    print.settlement = true;
    EXPECT_FALSE(map_trade(print, loaded(6), k_ingest).has_value());
}

TEST(PolymarketFunding, TickersCarryRateMarkIndexAndOpenInterest) {
    const auto frame = fixture("ws_tickers.json");
    ASSERT_EQ(parse_frame(frame), Frame::Ticker);
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    const auto tsla = loaded(21);
    const auto funding = map_funding(*wire, tsla, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 6'250);
    EXPECT_EQ(funding->mark_price.raw, 38'068);
    EXPECT_EQ(funding->index_price.raw, 38'049);
    EXPECT_EQ(funding->next_funding_time.ns, 1'791'302'400'000'000'000ULL);
    EXPECT_EQ(funding->ts_event.ns, 1'791'299'532'200'000'000ULL);
    EXPECT_EQ(funding->accrual, FundingAccrual::Discrete);

    const auto stats_frame = fixture("ws_statistics.json");
    ASSERT_EQ(parse_frame(stats_frame), Frame::Statistics);
    const auto statistics = parse_statistics(stats_frame);
    ASSERT_TRUE(statistics.has_value());
    const Stats stats{.volume_quote = *parse_turnover_e6(statistics->volume),
                      .open_interest = *parse_amount_e9(wire->open_interest)};
    const auto ticker = map_ticker(stats, tsla, k_ingest);
    EXPECT_EQ(ticker.open_interest.raw, 932'501'200'000);
    EXPECT_EQ(ticker.volume_24h_quote.raw, 879'767'794'668);
}

TEST(PolymarketControl, RefusalsAndTheClock) {
    const auto answer = fixture("ws_answer.json");
    EXPECT_EQ(parse_frame(answer), Frame::Answer);
    EXPECT_EQ(count_refusals(answer), 1U);
    EXPECT_EQ(count_refusals(fixture("ws_pong.json")), 0U);
    EXPECT_EQ(exec::json_number(fixture("rest_time.json"), "time", 1'000'000), 1'791'299'483'948'000'000ULL);
    EXPECT_EQ(to_string(VenueId::PolymarketPerps), "polymarketperps");
}

} // namespace
} // namespace raikiri::venues::polymarketperps
