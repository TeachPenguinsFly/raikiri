#include <raikiri/admin/registry.hpp>
#include <raikiri/admin/warm.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

using namespace raikiri;
using namespace raikiri::admin;

namespace {

Listing listing(std::string venue, double mid, double volume = 0.0, std::string market = "perp") {
    return {.instrument_id = "1",
            .account = venue,
            .venue = venue,
            .market = std::move(market),
            .exchange_symbol = "X",
            .local_symbol = "CAT/USDT",
            .volume_24h_usd = volume,
            .mid = mid};
}

Market market(std::string key, std::vector<Listing> listings) {
    double volume = 0.0;
    for (const auto& l : listings) {
        volume += l.volume_24h_usd;
    }
    return {.key = std::move(key), .symbol = "CAT/USDT", .listings = std::move(listings), .volume_24h_usd = volume};
}

const Market* find(const std::vector<Market>& markets, const std::string& key) {
    for (const auto& m : markets) {
        if (m.key == key) {
            return &m;
        }
    }
    return nullptr;
}

} // namespace

TEST(Jobs, RunsWhenDueAndSaysWhen) {
    Jobs jobs;
    int runs = 0;
    jobs.add("markets", std::chrono::milliseconds{100}, [&runs] { ++runs; });
    const auto ms = [](std::uint64_t n) { return MonoNanos{n * 1'000'000ULL}; };

    EXPECT_EQ(jobs.tick(ms(0)).ns, ms(100).ns); // due at once, then a period later
    EXPECT_EQ(runs, 1);
    (void)jobs.tick(ms(50));
    EXPECT_EQ(runs, 1);
    (void)jobs.tick(ms(100));
    EXPECT_EQ(runs, 2);
    jobs.run_now("markets", ms(120));
    EXPECT_EQ(runs, 3);

    const auto status = jobs.status(ms(150));
    ASSERT_EQ(status.size(), 1U);
    EXPECT_EQ(status[0].name, "markets");
    EXPECT_EQ(status[0].period_ms, 100U);
    EXPECT_EQ(status[0].runs, 3U);
    EXPECT_EQ(status[0].age_ms, 30);
}

TEST(Jobs, AnswersOnlyItsOwnPath) {
    Jobs jobs;
    jobs.add("markets", std::chrono::seconds{15}, [] {});
    EXPECT_FALSE(jobs.route(http::Request{.method = "GET", .path = "/cache/stats"}, MonoNanos{}).has_value());
    const auto answer = jobs.route(http::Request{.method = "GET", .path = "/cache/jobs"}, MonoNanos{});
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(answer->status, 200);
    EXPECT_NE(answer->body.find("\"markets\""), std::string::npos);
}

// three venues agree and a fourth quotes a different coin under the same ticker
TEST(SplitStrangers, MovesAListingFarFromTheMedianIntoItsOwnMarket) {
    std::vector<Market> markets{market("crypto:CAT/USD*",
                                       {listing("bybit", 0.000001, 10.0),
                                        listing("okx", 0.0000011, 20.0),
                                        listing("gate", 0.00000105, 30.0),
                                        listing("hyperliquid", 0.14, 5'000.0)})};
    split_strangers(markets);

    const auto* main = find(markets, "crypto:CAT/USD*");
    ASSERT_NE(main, nullptr);
    EXPECT_EQ(main->listings.size(), 3U);
    EXPECT_DOUBLE_EQ(main->volume_24h_usd, 60.0); // the stray's volume left with it
    const auto* stray = find(markets, "crypto:CAT/USD*@hyperliquid");
    ASSERT_NE(stray, nullptr);
    ASSERT_EQ(stray->listings.size(), 1U);
    EXPECT_EQ(stray->listings[0].venue, "hyperliquid");
    EXPECT_DOUBLE_EQ(stray->volume_24h_usd, 5'000.0);
}

// with two prices there is no median: the busier listing is the market
TEST(SplitStrangers, TwoPricesFollowTheBusierListing) {
    std::vector<Market> markets{
        market("crypto:HOLD/USD*", {listing("gate", 0.0000013, 900.0), listing("okx", 0.018, 50.0)})};
    split_strangers(markets);
    ASSERT_NE(find(markets, "crypto:HOLD/USD*@okx"), nullptr);
    EXPECT_EQ(find(markets, "crypto:HOLD/USD*")->listings.at(0).venue, "gate");
}

TEST(SplitStrangers, KeepsNearPricesUnquotedListingsAndOptions) {
    std::vector<Market> markets{market("crypto:BTC/USD*",
                                       {listing("binance", 64'500.0, 1e9),
                                        listing("okx", 64'480.0, 5e8),
                                        listing("bybit", 0.0, 0.0),                    // no quote yet
                                        listing("deribit", 2'300.0, 0.0, "option")})}; // a premium, not a price
    split_strangers(markets);
    ASSERT_EQ(markets.size(), 1U);
    EXPECT_EQ(markets[0].listings.size(), 4U);
    for (const auto& l : markets[0].listings) {
        EXPECT_FALSE(l.venue.empty()); // nothing left moved-from
    }
}

TEST(Rank, CrossListedFirstThenByTurnover) {
    std::vector<Market> markets{market("one-venue-huge", {listing("bitget", 1.0, 2e9)}),
                                market("two-venues-small", {listing("binance", 1.0, 1e6), listing("okx", 1.0, 1e6)}),
                                market("two-venues-big", {listing("binance", 1.0, 5e8), listing("okx", 1.0, 5e8)})};
    Registry::rank(markets);
    ASSERT_EQ(markets.size(), 3U);
    EXPECT_EQ(markets[0].key, "two-venues-big");
    EXPECT_EQ(markets[1].key, "two-venues-small");
    EXPECT_EQ(markets[2].key, "one-venue-huge");
}
