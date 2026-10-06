#include <raikiri/domain/asset_class.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/venues/vest/map.hpp>
#include <raikiri/venues/vest/wire.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>

namespace raikiri::venues::vest {
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

WireMarket row(std::string_view symbol) {
    static const auto body = fixture("rest_exchange_info.json");
    static const auto storage = std::make_unique<MarketBuffer>();
    const auto rows = parse_markets(body, *storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& one : *rows) {
        if (one.symbol == symbol) {
            return one;
        }
    }
    ADD_FAILURE() << "no market " << symbol;
    return {};
}

InstrumentBase loaded(std::string_view symbol) {
    auto instrument = map_instrument(row(symbol), MarketType::Perp);
    EXPECT_TRUE(instrument.has_value()) << symbol;
    if (!instrument.has_value()) {
        return {};
    }
    classify(VenueId::Vest, *instrument);
    return base_of(*instrument);
}

TEST(VestCatalogue, LoadsTradingRowsWithTheVenuesClass) {
    EXPECT_FALSE(map_instrument(row("DDOG-USD-PERP"), MarketType::Perp).has_value()) << "halted";

    const auto btc = loaded("BTC-PERP");
    EXPECT_EQ(btc.local_symbol.view(), "BTC/USDC");
    EXPECT_EQ(btc.tick_size.raw, 10'000'000);
    EXPECT_EQ(btc.lot_size.raw, 100'000);
    EXPECT_EQ(btc.taker_fee.raw, 100'000);
    EXPECT_EQ(asset_key(btc), "crypto:BTC");

    const auto nvda = loaded("NVDA-USD-PERP");
    EXPECT_EQ(nvda.local_symbol.view(), "NVDA/USDC");
    EXPECT_EQ(asset_key(nvda), "equity:NVDA");
    EXPECT_EQ(asset_key(loaded("SPY-USD-PERP")), "equity:SPY");

    const auto eur = loaded("EUR-USD-PERP");
    EXPECT_EQ(eur.tick_size.raw, 1'000);
    EXPECT_EQ(eur.lot_size.raw, 1'000'000'000);
    EXPECT_EQ(asset_key(eur), "fx:EURUSD");
    EXPECT_EQ(asset_key(loaded("JPY-USD-PERP")), "fx:JPYUSD");
    EXPECT_EQ(asset_key(loaded("NOK-USD-PERP")), "fx:NOKUSD");
    EXPECT_EQ(asset_key(loaded("NOK-PERP")), "equity:NOK");
}

TEST(VestBook, EveryDepthFrameIsTheWholeBookAndItsTopIsTheQuote) {
    const auto frame = fixture("ws_depth.json");
    ASSERT_EQ(parse_channel(frame), Channel::Depth);
    LevelBuffer storage;
    const auto wire = parse_book(frame, storage);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "BTC-PERP");
    EXPECT_EQ(wire->ts_ms, 1'791'299'283'733U);
    const auto btc = loaded("BTC-PERP");
    const auto book = map_book(*wire, btc, k_ingest);
    ASSERT_TRUE(book.has_value());
    EXPECT_GT(book->bid_count, 10U);
    EXPECT_EQ(book->bids[0].price.raw, 8'668'500);
    EXPECT_EQ(book->bids[0].qty.raw, 3'420);
    EXPECT_EQ(book->ts_event.ns, 1'791'299'283'733'000'000ULL);
    const auto top = top_of(*book);
    ASSERT_TRUE(top.has_value());
    EXPECT_EQ(top->bid.raw, 8'668'500);
    EXPECT_GT(top->ask.raw, top->bid.raw);
}

TEST(VestTrades, TheTakersSideAndTheEnginesTime) {
    const auto frame = fixture("ws_trades.json");
    ASSERT_EQ(parse_channel(frame), Channel::Trades);
    const auto wire = parse_trade(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "BTC-PERP");
    const auto trade = map_trade(*wire, loaded("BTC-PERP"), k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->price.raw, 8'676'200);
    EXPECT_EQ(trade->qty.raw, 212);
    EXPECT_EQ(trade->ts_event.ns, 1'791'299'539'424'000'000ULL);
}

TEST(VestFunding, HourlyRateWithMarkAndTheNextHour) {
    const auto body = fixture("rest_ticker_latest.json");
    const auto storage = std::make_unique<TickerBuffer>();
    const auto rows = parse_tickers(body, *storage);
    ASSERT_TRUE(rows.has_value());
    const WireTicker* btc_row = nullptr;
    for (const auto& one : *rows) {
        if (one.symbol == "BTC-PERP") {
            btc_row = &one;
        }
    }
    ASSERT_NE(btc_row, nullptr);
    const auto funding = map_funding(*btc_row, loaded("BTC-PERP"), k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 410);
    EXPECT_EQ(funding->mark_price.raw, 8'653'104);
    EXPECT_EQ(funding->index_price.raw, 8'658'001);
    EXPECT_EQ(funding->interval_s, 3600U);
    EXPECT_EQ(funding->accrual, FundingAccrual::Discrete);
    EXPECT_EQ(funding->next_funding_time.ns, 1'791'302'400'000'000'000ULL);
    EXPECT_EQ(funding->ts_event.ns, 0U);
}

TEST(VestTicker, VolumeAndOpenInterestOfBothSides) {
    StatBuffer volumes;
    const auto vol = parse_volumes(fixture("rest_ticker_24hr.json"), volumes);
    ASSERT_TRUE(vol.has_value());
    StatBuffer interest;
    const auto oi = parse_open_interest(fixture("rest_oi.json"), interest);
    ASSERT_TRUE(oi.has_value());
    Stats stats{};
    for (const auto& one : *vol) {
        if (one.symbol == "BTC-PERP") {
            stats.volume_base = parse_amount_e9(one.first).value_or(FixedE9{});
            stats.volume_quote = parse_turnover_e6(one.second).value_or(FixedE6{});
        }
    }
    for (const auto& one : *oi) {
        if (one.symbol == "BTC-PERP") {
            stats.open_interest = FixedE9::from_raw(parse_amount_e9(one.first)->raw + parse_amount_e9(one.second)->raw);
        }
    }
    const auto ticker = map_ticker(stats, loaded("BTC-PERP"), k_ingest);
    EXPECT_EQ(ticker.volume_24h_quote.raw, 44'499'772'228'800);
    EXPECT_EQ(ticker.volume_24h_base.raw, 513'166'800'000);
    EXPECT_EQ(ticker.open_interest.raw, 18'317'100'000 + 21'946'100'000);
}

TEST(VestControl, SubscribeRefusalsPongAndClock) {
    const auto subscribed = fixture("ws_subscribe.json");
    EXPECT_EQ(parse_channel(subscribed), Channel::Subscribed);
    EXPECT_EQ(count_refusals(subscribed), 1U);
    EXPECT_EQ(parse_channel(fixture("ws_pong.json")), Channel::Pong);
    EXPECT_EQ(parse_channel(fixture("ws_ping_ack.json")), Channel::Unknown);
    EXPECT_EQ(exec::json_number(fixture("rest_ntp.json"), "timestamp", 1'000'000), 1'791'299'236'574'000'000ULL);
}

TEST(VestCaps, MarketDataOnly) {
    const auto& caps = exec::caps_of(VenueId::Vest);
    EXPECT_TRUE(caps.markets.perp);
    EXPECT_FALSE(caps.cancel_by_client_order_id);
    EXPECT_EQ(to_string(VenueId::Vest), "vest");
}

} // namespace
} // namespace raikiri::venues::vest
