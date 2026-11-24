#include <raikiri/candles/metrics.hpp>

#include <gtest/gtest.h>

#include <vector>

using namespace raikiri;
using namespace raikiri::candles;

namespace {

const InstrumentId k_btc{72339069014638592ULL}; // binance perp

MetricSample at(std::int32_t minute) {
    return {.instrument = k_btc, .minute = minute};
}

} // namespace

TEST(MetricInterval, ReadsTheChartIntervalsAndNothingElse) {
    EXPECT_EQ(metric_interval("5m"), 5);
    EXPECT_EQ(metric_interval("15m"), 15);
    EXPECT_EQ(metric_interval("1h"), 60);
    EXPECT_EQ(metric_interval("4h"), 240);
    EXPECT_EQ(metric_interval("1d"), 1440);
    EXPECT_FALSE(metric_interval("1m").has_value());
    EXPECT_FALSE(metric_interval("30m").has_value());
    EXPECT_FALSE(metric_interval("").has_value());
    EXPECT_FALSE(metric_interval("5M").has_value());
}

TEST(MetricLimit, DefaultsAndClampsToAThousand) {
    EXPECT_EQ(metric_limit(std::nullopt), 1000U);
    EXPECT_EQ(metric_limit(0), 0U);
    EXPECT_EQ(metric_limit(1), 1U);
    EXPECT_EQ(metric_limit(1000), 1000U);
    EXPECT_EQ(metric_limit(1001), 1000U);
    EXPECT_EQ(metric_limit(~0ULL), 1000U);
}

TEST(MetricWindow, EndsAfterTheBucketNowFallsInAndSpansTheLimit) {
    // minute 1003 at 5m sits in the bucket [1000, 1005)
    const auto w = metric_window(1003, 5, 3);
    EXPECT_EQ(w.to, 1005);
    EXPECT_EQ(w.from, 990);
    const auto edge = metric_window(1000, 5, 1);
    EXPECT_EQ(edge.from, 1000);
    EXPECT_EQ(edge.to, 1005);
    const auto none = metric_window(1003, 5, 0);
    EXPECT_EQ(none.from, none.to);
}

TEST(BucketMetrics, KeepsTheLastSampleOfEachFieldThatHadOne) {
    auto a = at(1000);
    a.funding_rate = 100;
    a.mark = 50'000;
    a.open_interest = 7;
    auto b = at(1002);
    b.funding_rate = 110;
    b.index = 49'990;
    auto c = at(1004);
    c.mark = 50'010;
    const std::vector<MetricSample> samples{a, b, c};
    const auto out = bucket_metrics(samples, 5);
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].instrument, k_btc);
    EXPECT_EQ(out[0].minute, 1000);
    EXPECT_EQ(out[0].funding_rate, 110);
    EXPECT_EQ(out[0].mark, 50'010);
    EXPECT_EQ(out[0].index, 49'990);
    EXPECT_EQ(out[0].open_interest, 7);
}

TEST(BucketMetrics, LeavesAFieldEmptyWhenNoSampleInTheBucketHadIt) {
    auto a = at(1000);
    a.mark = 1;
    auto b = at(1005);
    b.funding_rate = 2;
    const std::vector<MetricSample> samples{a, b};
    const auto out = bucket_metrics(samples, 5);
    ASSERT_EQ(out.size(), 2U);
    EXPECT_EQ(out[0].mark, 1);
    EXPECT_FALSE(out[0].funding_rate.has_value());
    EXPECT_FALSE(out[0].open_interest.has_value());
    EXPECT_EQ(out[1].minute, 1005);
    EXPECT_EQ(out[1].funding_rate, 2);
    EXPECT_FALSE(out[1].mark.has_value());
}

TEST(BucketMetrics, OpensBucketsOnTheIntervalAndSkipsEmptyOnes) {
    auto a = at(59);
    a.mark = 1;
    auto b = at(60);
    b.mark = 2;
    auto c = at(250);
    c.mark = 3;
    const std::vector<MetricSample> samples{a, b, c};
    const auto out = bucket_metrics(samples, 60);
    ASSERT_EQ(out.size(), 3U);
    EXPECT_EQ(out[0].minute, 0);
    EXPECT_EQ(out[1].minute, 60);
    EXPECT_EQ(out[2].minute, 240);
    EXPECT_TRUE(bucket_metrics({}, 60).empty());
}

TEST(MetricSample, IsEmptyUntilAFieldIsSet) {
    auto s = at(1);
    EXPECT_TRUE(s.empty());
    s.open_interest = 0;
    EXPECT_FALSE(s.empty());
}
