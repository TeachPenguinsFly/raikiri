#include <raikiri/introspect/clock_probe.hpp>
#include <raikiri/introspect/feed_window.hpp>
#include <raikiri/introspect/latency.hpp>
#include <raikiri/introspect/offset_bounds.hpp>
#include <raikiri/introspect/routes.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <deque>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace raikiri::introspect {
namespace {

constexpr std::uint64_t k_ms = 1'000'000;
constexpr std::uint64_t k_s = 1'000'000'000;

TEST(BestOfBurst, CentresWhereEverySampleAgrees) {
    EXPECT_FALSE(best_of_burst({}).has_value());
    // each sample puts the offset in [server - received, server - sent]: [-10, 90], [-5, 15] and [-40, 10] ms
    const std::vector<OffsetSample> burst{
        {.sent_ns = 1000 * k_ms, .received_ns = 1100 * k_ms, .server_ns = 1090 * k_ms, .resolution_ns = 0},
        {.sent_ns = 2000 * k_ms, .received_ns = 2020 * k_ms, .server_ns = 2015 * k_ms, .resolution_ns = 0},
        {.sent_ns = 3000 * k_ms, .received_ns = 3050 * k_ms, .server_ns = 3010 * k_ms, .resolution_ns = 0}};
    const auto e = best_of_burst(burst);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->method, ClockMethod::RestProbe);
    EXPECT_EQ(e->offset_ns, static_cast<std::int64_t>(2'500'000));
    EXPECT_EQ(e->uncertainty_ns, 7'500'000U) << "narrower than the shortest trip's 10 ms alone";
    EXPECT_EQ(e->rtt_ns, 20 * k_ms);
    EXPECT_EQ(e->sampled_ns, 2020 * k_ms);
    EXPECT_EQ(e->samples, 3U);
}

TEST(BestOfBurst, SamplesThatCannotAllBeTrueFallBackToTheShortestTrip) {
    // [0, 10] and [20, 60] ms: two clocks answer under one name
    const std::vector<OffsetSample> burst{
        {.sent_ns = 1000 * k_ms, .received_ns = 1010 * k_ms, .server_ns = 1010 * k_ms, .resolution_ns = 0},
        {.sent_ns = 2000 * k_ms, .received_ns = 2040 * k_ms, .server_ns = 2060 * k_ms, .resolution_ns = 0}};
    const auto e = best_of_burst(burst);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->offset_ns, static_cast<std::int64_t>(5 * k_ms));
    EXPECT_EQ(e->uncertainty_ns, 5 * k_ms);
    EXPECT_EQ(e->rtt_ns, 10 * k_ms);
}

TEST(BestOfBurst, ANegativeOffsetAndHalfTheStampResolution) {
    // the venue is behind: it stamps 99 ms at a 102 ms midpoint, and a ms stamp means up to 1 ms more
    const std::vector<OffsetSample> burst{
        {.sent_ns = 100 * k_ms, .received_ns = 104 * k_ms, .server_ns = 99 * k_ms, .resolution_ns = k_ms}};
    const auto e = best_of_burst(burst);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->offset_ns, -static_cast<std::int64_t>(2'500'000));
    EXPECT_EQ(e->uncertainty_ns, 2'500'000U);
    EXPECT_EQ(e->resolution_ns, k_ms);
}

TEST(BestOfBurst, SkipsBackwardsAndUnstampedSamples) {
    const std::vector<OffsetSample> burst{{.sent_ns = 10, .received_ns = 5, .server_ns = 7, .resolution_ns = 0},
                                          {.sent_ns = 10, .received_ns = 11, .server_ns = 0, .resolution_ns = 0}};
    EXPECT_FALSE(best_of_burst(burst).has_value());
}

class ScriptedSource final : public ServerTimeSource {
public:
    std::deque<std::optional<OffsetSample>> answers;
    int reads{0};

    [[nodiscard]] std::string endpoint() const override { return "test/time"; }

    [[nodiscard]] std::optional<OffsetSample> read() override {
        ++reads;
        if (answers.empty()) {
            return std::nullopt;
        }
        auto next = answers.front();
        answers.pop_front();
        return next;
    }
};

TEST(ClockSync, BurstsAtStartThenHourlyAndReportsDrift) {
    ScriptedSource source;
    const auto sample = [](std::uint64_t at, std::uint64_t rtt, std::int64_t offset) {
        return OffsetSample{.sent_ns = at,
                            .received_ns = at + rtt,
                            .server_ns = static_cast<std::uint64_t>(static_cast<std::int64_t>(at + rtt / 2) + offset),
                            .resolution_ns = 0};
    };
    source.answers = {sample(k_s, 40 * k_ms, 0),
                      std::nullopt,
                      sample(2 * k_s, 8 * k_ms, 3 * static_cast<std::int64_t>(k_ms)),
                      sample(3 * k_s, 30 * k_ms, 0),
                      sample(4 * k_s, 9 * k_ms, 0)};
    std::uint64_t now = 0;
    int pauses = 0;
    ClockSync sync{source,
                   [&] { return now; },
                   [] { return true; },
                   [&](std::uint64_t) {
                       ++pauses;
                       return true;
                   }};
    EXPECT_EQ(sync.estimate().method, ClockMethod::None);
    EXPECT_EQ(sync.tick(), ProbeSchedule{}.period_ns);
    EXPECT_EQ(source.reads, 8);
    EXPECT_EQ(pauses, 7);
    // [-20, 20], [-1, 7], [-15, 15] and [-4.5, 4.5] ms meet in [-1, 4.5]
    EXPECT_EQ(sync.estimate().offset_ns, static_cast<std::int64_t>(1'750'000));
    EXPECT_EQ(sync.estimate().uncertainty_ns, 2'750'000U);
    EXPECT_EQ(sync.estimate().rtt_ns, 8 * k_ms);
    EXPECT_EQ(sync.estimate().samples, 4U);
    EXPECT_EQ(sync.estimate().drift_ns, 0);

    now = 10 * k_s;
    EXPECT_EQ(sync.tick(), ProbeSchedule{}.period_ns);
    EXPECT_EQ(source.reads, 8);

    now = ProbeSchedule{}.period_ns;
    source.answers = {sample(now, 6 * k_ms, -static_cast<std::int64_t>(k_ms))};
    EXPECT_EQ(sync.tick(), 2 * ProbeSchedule{}.period_ns);
    EXPECT_EQ(sync.estimate().offset_ns, -static_cast<std::int64_t>(k_ms));
    EXPECT_EQ(sync.estimate().drift_ns, -static_cast<std::int64_t>(2'750'000));
    EXPECT_EQ(sync.estimate().drift_over_ns, now + 6 * k_ms - (2 * k_s + 8 * k_ms));
}

TEST(ClockSync, ASilentVenueIsRetriedAndHonoursTheBudget) {
    ScriptedSource source;
    std::uint64_t now = 0;
    bool admit = false;
    ClockSync sync{source, [&] { return now; }, [&] { return admit; }, [](std::uint64_t) { return true; }};
    EXPECT_EQ(sync.tick(), ProbeSchedule{}.retry_ns);
    EXPECT_EQ(source.reads, 0);
    EXPECT_EQ(sync.failures(), 1U);
    admit = true;
    now = ProbeSchedule{}.retry_ns;
    EXPECT_EQ(sync.tick(), 2 * ProbeSchedule{}.retry_ns);
    EXPECT_EQ(source.reads, 8);
    EXPECT_EQ(sync.estimate().method, ClockMethod::None);
}

TEST(ClockSync, StopsTheBurstWhenTheCallerStops) {
    ScriptedSource source;
    ClockSync sync{source, [] { return std::uint64_t{0}; }, [] { return true; }, [](std::uint64_t) { return false; }};
    (void)sync.tick();
    EXPECT_EQ(source.reads, 1);
}

TEST(LagHistogram, BucketsBothSignsAndReadsPercentiles) {
    EXPECT_EQ(LagHistogram::index_of(0), LagHistogram::k_side);
    EXPECT_EQ(LagHistogram::index_of(999), LagHistogram::k_side);
    EXPECT_EQ(LagHistogram::index_of(-999), LagHistogram::k_side);
    EXPECT_GT(LagHistogram::index_of(1000), LagHistogram::k_side);
    EXPECT_LT(LagHistogram::index_of(-1000), LagHistogram::k_side);
    EXPECT_EQ(LagHistogram::index_of(std::numeric_limits<std::int64_t>::max()), LagHistogram::k_buckets - 1);
    EXPECT_EQ(LagHistogram::index_of(std::numeric_limits<std::int64_t>::min()), 0U);
    for (const std::int64_t v : {1'500LL, 7'300LL, 1'000'000LL, 45'000'000LL, 3'000'000'000LL}) {
        const auto mid = LagHistogram::middle_of(LagHistogram::index_of(v));
        EXPECT_LE(std::abs(mid - v) * 8, v) << v;
        EXPECT_EQ(LagHistogram::middle_of(LagHistogram::index_of(-v)), -mid);
    }

    LagHistogram h;
    EXPECT_EQ(h.quantile(500), 0);
    for (int i = 0; i < 98; ++i) {
        h.record(static_cast<std::int64_t>(2 * k_ms));
    }
    h.record(-static_cast<std::int64_t>(3 * k_ms));
    h.record(static_cast<std::int64_t>(400 * k_ms));
    EXPECT_EQ(h.count(), 100U);
    EXPECT_NEAR(static_cast<double>(h.quantile(500)), 2e6, 2e6 / 8);
    EXPECT_NEAR(static_cast<double>(h.quantile(990)), 2e6, 2e6 / 8);
    EXPECT_NEAR(static_cast<double>(h.quantile(1000)), 400e6, 400e6 / 8);
    EXPECT_NEAR(static_cast<double>(h.quantile(1)), -3e6, 3e6 / 8);
}

TEST(LagHistogram, ReadsInsideABucketInsteadOfItsMiddle) {
    // 2100..2595 ms all land in the bucket from 2097 to 2621 ms
    LagHistogram h;
    for (std::uint64_t i = 0; i < 100; ++i) {
        h.record(static_cast<std::int64_t>((2100 + 5 * i) * k_ms));
    }
    EXPECT_NEAR(static_cast<double>(h.quantile(500)), 2347.5e6, 30e6);
    EXPECT_NEAR(static_cast<double>(h.quantile(990)), 2590e6, 30e6);

    LagHistogram negative;
    for (std::uint64_t i = 0; i < 100; ++i) {
        negative.record(-static_cast<std::int64_t>((2100 + 5 * i) * k_ms));
    }
    EXPECT_NEAR(static_cast<double>(negative.quantile(10)), -2590e6, 35e6);
    EXPECT_NEAR(static_cast<double>(negative.quantile(500)), -2347.5e6, 30e6);
}

TEST(FeedWindow, CountsAMinuteAndForgetsOlderSlices) {
    FeedWindow window;
    window.advance(k_s);
    window.record(static_cast<std::int64_t>(5 * k_ms), 100);
    window.count(200);
    window.advance(11 * k_s);
    window.record(-static_cast<std::int64_t>(1 * k_ms), 300);
    auto s = window.summary(21 * k_s);
    EXPECT_EQ(s.messages, 3U);
    EXPECT_EQ(s.lag_samples, 2U);
    EXPECT_EQ(s.window_ns, 20 * k_s);
    EXPECT_EQ(s.last_recv_ns, 300U);
    EXPECT_GT(s.lag_p99_ns, 0);
    EXPECT_LT(s.lag_p50_ns, 0);

    window.advance(61 * k_s);
    s = window.summary(61 * k_s);
    EXPECT_EQ(s.messages, 1U);
    EXPECT_EQ(s.window_ns, 50 * k_s);

    window.advance(200 * k_s);
    s = window.summary(205 * k_s);
    EXPECT_EQ(s.messages, 0U);
    EXPECT_EQ(s.window_ns, 5 * k_s);
    EXPECT_EQ(s.last_recv_ns, 300U);
}

TEST(FeedWindow, KeepsTheExactSmallestLagForAMinute) {
    FeedWindow window;
    EXPECT_FALSE(window.lag_min().has_value());
    window.advance(k_s);
    window.count(50);
    EXPECT_FALSE(window.lag_min().has_value()) << "unstamped messages bound nothing";
    window.record(static_cast<std::int64_t>(7 * k_ms), 100);
    window.record(-static_cast<std::int64_t>(1'234'567), 110);
    window.advance(11 * k_s);
    window.record(static_cast<std::int64_t>(3 * k_ms), 300);
    EXPECT_EQ(window.lag_min(), -static_cast<std::int64_t>(1'234'567));
    EXPECT_EQ(window.summary(12 * k_s).lag_min_ns, -static_cast<std::int64_t>(1'234'567));

    window.advance(61 * k_s);
    EXPECT_EQ(window.lag_min(), static_cast<std::int64_t>(3 * k_ms)) << "the first slice has rolled out";
    window.advance(200 * k_s);
    EXPECT_FALSE(window.lag_min().has_value());
}

TEST(OffsetInterval, IntersectsAndReportsAnEmptyMeet) {
    const OffsetInterval probe{.lo = -9, .hi = -2};
    EXPECT_EQ(intersect(probe, {}), probe) << "an open interval takes nothing away";
    EXPECT_EQ(intersect(probe, {.lo = -5, .hi = k_open_above}), (OffsetInterval{.lo = -5, .hi = -2}));
    const auto apart = intersect(probe, {.lo = 1, .hi = 4});
    EXPECT_TRUE(apart.empty());
    EXPECT_EQ(apart, (OffsetInterval{.lo = 1, .hi = -2})) << "both edges stay visible";
    EXPECT_FALSE(intersect(probe, {.lo = -2, .hi = 0}).empty()) << "touching edges still meet";
}

TEST(OffsetInterval, ASampleSpansSendToReceivePlusOneTick) {
    // the venue says 99 ms on a millisecond clock while we sent at 100 and heard back at 104
    const auto i = sample_interval(
        {.sent_ns = 100 * k_ms, .received_ns = 104 * k_ms, .server_ns = 99 * k_ms, .resolution_ns = k_ms});
    EXPECT_EQ(i.lo, -5 * static_cast<std::int64_t>(k_ms));
    EXPECT_EQ(i.hi, 0);
    const auto e = best_of_burst(std::vector<OffsetSample>{
        {.sent_ns = 100 * k_ms, .received_ns = 104 * k_ms, .server_ns = 99 * k_ms, .resolution_ns = k_ms}});
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(rest_interval(*e), i);
    EXPECT_EQ(rest_interval(ClockEstimate{}), OffsetInterval{});
}

TEST(LeastCorrection, MovesOurClockOnlyAsFarAsTheBoundsForce) {
    EXPECT_EQ(least_correction({.lo = -3, .hi = 8}), 0) << "an interval holding zero is no evidence of an offset";
    EXPECT_EQ(least_correction({.lo = 2, .hi = 8}), 2);
    EXPECT_EQ(least_correction({.lo = -8, .hi = -2}), -2);
    EXPECT_EQ(least_correction({.lo = 5, .hi = k_open_above}), 5);
    EXPECT_EQ(least_correction({.lo = k_open_below, .hi = -1}), -1);
    EXPECT_EQ(least_correction({}), 0);
    EXPECT_EQ(least_correction({.lo = 4, .hi = 1}), 0) << "a conflict corrects nothing";
}

TEST(CombineOffset, TheFeedTightensTheProbeAndStandsAloneWithoutOne) {
    const auto ms = [](std::int64_t n) { return n * static_cast<std::int64_t>(k_ms); };
    const ClockEstimate probe{.method = ClockMethod::RestProbe, .offset_ns = ms(5), .uncertainty_ns = 13 * k_ms};

    EXPECT_EQ(combine(ClockEstimate{}, std::nullopt).source, OffsetSource::None);

    auto c = combine(probe, std::nullopt);
    EXPECT_EQ(c.source, OffsetSource::Rest);
    EXPECT_EQ(c.interval, (OffsetInterval{.lo = ms(-8), .hi = ms(18)}));
    EXPECT_EQ(c.correction_ns, 0) << "|5| < 13: not significant";

    c = combine(probe, ms(-20));
    EXPECT_EQ(c.source, OffsetSource::Rest) << "a looser feed bound adds nothing";
    EXPECT_EQ(c.interval.lo, ms(-8));

    c = combine(probe, ms(3));
    EXPECT_EQ(c.source, OffsetSource::RestAndFeed);
    EXPECT_EQ(c.interval, (OffsetInterval{.lo = ms(3), .hi = ms(18)}));
    EXPECT_EQ(c.correction_ns, ms(3));

    c = combine(probe, ms(25));
    EXPECT_EQ(c.source, OffsetSource::Conflict);
    EXPECT_TRUE(c.interval.empty());
    EXPECT_EQ(c.interval, (OffsetInterval{.lo = ms(25), .hi = ms(18)}));
    EXPECT_EQ(c.correction_ns, 0);

    const ClockEstimate behind{.method = ClockMethod::RestProbe, .offset_ns = ms(-30), .uncertainty_ns = 4 * k_ms};
    c = combine(behind, ms(-40));
    EXPECT_EQ(c.source, OffsetSource::Rest);
    EXPECT_EQ(c.correction_ns, ms(-26)) << "a venue clearly behind: the edge nearest zero";

    c = combine(ClockEstimate{}, ms(2));
    EXPECT_EQ(c.source, OffsetSource::Feed);
    EXPECT_EQ(c.interval, (OffsetInterval{.lo = ms(2), .hi = k_open_above}));
    EXPECT_EQ(c.correction_ns, ms(2));
    EXPECT_EQ(combine(ClockEstimate{}, ms(-2)).correction_ns, 0);
}

TEST(CombineOffset, CorrectedLagsInTheWindowAreNeverNegative) {
    FeedWindow window;
    window.advance(k_s);
    for (const std::int64_t lag : {4'000'000, -1'500'000, 900'000}) {
        window.record(lag, 1);
    }
    const auto bound = -*window.lag_min();
    const auto c = combine(ClockEstimate{.method = ClockMethod::RestProbe, .offset_ns = 0, .uncertainty_ns = 20 * k_ms},
                           bound);
    EXPECT_EQ(c.correction_ns, 1'500'000);
    EXPECT_EQ(*window.lag_min() + c.correction_ns, 0);
}

TEST(LatencySeries, BucketsCountEverythingSinceStart) {
    LatencySeries series;
    series.record(500);
    series.record(static_cast<std::int64_t>(2 * k_ms));
    series.rotate();
    series.rotate();
    series.record(static_cast<std::int64_t>(20 * k_s));
    const auto s = series.stats("rest", Stage::Rest);
    ASSERT_EQ(s.buckets.size(), k_bucket_bounds_ns.size());
    EXPECT_EQ(k_bucket_bounds_ns.front(), 1'000U);
    EXPECT_EQ(k_bucket_bounds_ns[15], 1'000'000U);
    EXPECT_EQ(k_bucket_bounds_ns.back(), 10 * k_s);
    EXPECT_EQ(s.buckets[0], 1U);
    EXPECT_EQ(s.buckets[15], 1U);
    EXPECT_EQ(s.buckets[17], 2U);
    EXPECT_EQ(s.buckets.back(), 2U);
    EXPECT_EQ(s.total, 3U);
    EXPECT_EQ(s.count, 1U);
}

TEST(LatencySeries, ClampsNegativesAndKeepsTwoSlices) {
    LatencySeries series;
    series.record(-5);
    series.record(static_cast<std::int64_t>(10 * k_ms));
    series.rotate();
    series.record(static_cast<std::int64_t>(20 * k_ms));
    auto s = series.stats("bbo", Stage::Feed);
    EXPECT_EQ(s.stage, "feed");
    EXPECT_EQ(s.count, 3U);
    EXPECT_EQ(s.total, 3U);
    EXPECT_EQ(s.below_zero, 1U);
    EXPECT_EQ(s.max_ns, 20 * k_ms);
    EXPECT_EQ(s.sum_ns, 30 * k_ms);
    series.rotate();
    series.rotate();
    s = series.stats("bbo", Stage::Feed);
    EXPECT_EQ(s.count, 0U);
    EXPECT_EQ(s.total, 3U);
}

TEST(LatencyBook, FindsASeriesOnceAndRotatesOnTheSlice) {
    LatencyBook book;
    auto& a = book.series("bbo", Stage::Feed);
    EXPECT_EQ(&a, &book.series("bbo", Stage::Feed));
    EXPECT_NE(&a, &book.series("bbo", Stage::Internal));
    a.record(5);
    book.rotate_if_due(0);
    book.rotate_if_due(LatencyBook::k_slice_ns);
    book.rotate_if_due(2 * LatencyBook::k_slice_ns);
    const auto stats = book.stats();
    ASSERT_EQ(stats.size(), 2U);
    EXPECT_EQ(stats[0].count, 0U);
    EXPECT_EQ(stats[0].total, 1U);
}

class OneSource final : public GatewayStatsSource {
public:
    std::map<std::string, SnapshotRecord> records;

    [[nodiscard]] std::vector<SnapshotRecord> list() const override {
        std::vector<SnapshotRecord> out;
        for (const auto& [name, record] : records) {
            out.push_back(record);
        }
        return out;
    }

    [[nodiscard]] std::optional<SnapshotRecord> read(std::string_view account) const override {
        const auto it = records.find(std::string{account});
        return it == records.end() ? std::nullopt : std::optional{it->second};
    }
};

http::Request get(std::string_view path) {
    http::Request req{};
    req.method = "GET";
    req.path = path;
    return req;
}

class ExportTest : public ::testing::Test {
protected:
    void SetUp() override {
        GatewayStats s{};
        s.account = "okx";
        s.venue = "okx";
        s.taken_ns = 100 * k_s;
        s.clock = ClockStats{.venue = "okx",
                             .method = "rest_probe",
                             .endpoint = "www.okx.com/api/v5/public/time",
                             .offset_ns = -1'500'000,
                             .uncertainty_ns = 2'000'000,
                             .rtt_ns = 3'000'000,
                             .sampled_ns = 98 * k_s,
                             .samples = 5};
        s.latency.push_back(LatencyStats{.channel = "bbo",
                                         .stage = "feed",
                                         .count = 10,
                                         .p50_ns = 4 * k_ms,
                                         .p90_ns = 6 * k_ms,
                                         .p99_ns = 9 * k_ms,
                                         .p999_ns = 9 * k_ms,
                                         .max_ns = 9 * k_ms,
                                         .total = 40,
                                         .sum_ns = 200 * k_ms,
                                         .below_zero = 1,
                                         .buckets = {}});
        s.latency.back().buckets.assign(k_bucket_bounds_ns.size(), 40);
        s.latency.back().buckets[0] = 1;
        source_.records["okx"] = {.stats = s, .process_alive = true};
    }

    OneSource source_;
    GetMachineStats machine_{source_, [] { return 100 * k_s; }};
    GetGatewayStats gateway_{source_, [] { return 100 * k_s; }};
};

TEST_F(ExportTest, LatencyRoutesServeTheSections) {
    const CacheRoutes routes{machine_, gateway_};
    const auto all = routes(get("/cache/latency"));
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all->status, 200);
    EXPECT_NE(all->body.find(R"("offset_ns":-1500000)"), std::string::npos);
    EXPECT_NE(all->body.find(R"("sampled_ns":"98000000000")"), std::string::npos);
    EXPECT_NE(all->body.find(R"("stage":"feed")"), std::string::npos);
    const auto one = routes(get("/cache/latency/okx"));
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(one->status, 200);
    EXPECT_EQ(routes(get("/cache/latency/nobody"))->status, 404);
}

TEST_F(ExportTest, MetricsAreGroupedPrometheusText) {
    const MetricsRoute route{machine_};
    EXPECT_FALSE(route(get("/cache/stats")).has_value());
    const auto answer = route(get("/metrics"));
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(answer->content_type, "text/plain; version=0.0.4; charset=utf-8");
    const auto& body = answer->body;
    EXPECT_NE(body.find(R"(raikiri_clock_offset_seconds{account="okx",venue="okx",method="rest_probe"} -0.0015)"),
              std::string::npos);
    EXPECT_NE(
        body.find(
            R"(raikiri_latency_seconds_bucket{account="okx",venue="okx",channel="bbo",stage="feed",le="1e-06"} 1)"),
        std::string::npos);
    EXPECT_NE(
        body.find(
            R"(raikiri_latency_seconds_bucket{account="okx",venue="okx",channel="bbo",stage="feed",le="0.001585"} 40)"),
        std::string::npos);
    EXPECT_NE(
        body.find(
            R"(raikiri_latency_seconds_bucket{account="okx",venue="okx",channel="bbo",stage="feed",le="+Inf"} 40)"),
        std::string::npos);
    EXPECT_NE(body.find("# TYPE raikiri_latency_seconds histogram"), std::string::npos);
    EXPECT_NE(body.find(R"(raikiri_latency_seconds_count{account="okx",venue="okx",channel="bbo",stage="feed"} 40)"),
              std::string::npos);
    EXPECT_NE(body.find(R"(raikiri_clock_sample_age_seconds{account="okx",venue="okx",method="rest_probe"} 2)"),
              std::string::npos);
    std::map<std::string, int> types;
    std::size_t at = 0;
    while ((at = body.find("# TYPE ", at)) != std::string::npos) {
        at += 7;
        ++types[body.substr(at, body.find(' ', at) - at)];
    }
    for (const auto& [name, seen] : types) {
        EXPECT_EQ(seen, 1) << name;
    }
}

} // namespace
} // namespace raikiri::introspect
