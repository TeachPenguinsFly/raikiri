#include <raikiri/introspect/clock_probe.hpp>
#include <raikiri/introspect/latency.hpp>
#include <raikiri/introspect/routes.hpp>
#include <raikiri/introspect/server_time.hpp>

#include <gtest/gtest.h>

#include <deque>
#include <map>
#include <string>
#include <vector>

namespace raikiri::introspect {
namespace {

constexpr std::uint64_t k_ms = 1'000'000;
constexpr std::uint64_t k_s = 1'000'000'000;

TEST(OffsetEstimator, KeepsTheShortestRoundTrip) {
    OffsetEstimator est;
    EXPECT_EQ(est.estimate().method, ClockMethod::None);
    // server 5 ms ahead; the slow probe's asymmetric queueing skews it
    EXPECT_TRUE(
        est.add({.sent_ns = 1000 * k_ms, .received_ns = 1100 * k_ms, .server_ns = 1090 * k_ms, .resolution_ns = 0}));
    EXPECT_TRUE(
        est.add({.sent_ns = 2000 * k_ms, .received_ns = 2010 * k_ms, .server_ns = 2010 * k_ms, .resolution_ns = 0}));
    const auto e = est.estimate();
    EXPECT_EQ(e.method, ClockMethod::RestProbe);
    EXPECT_EQ(e.offset_ns, static_cast<std::int64_t>(5 * k_ms));
    EXPECT_EQ(e.rtt_ns, 10 * k_ms);
    EXPECT_EQ(e.uncertainty_ns, 5 * k_ms);
    EXPECT_EQ(e.samples, 2U);
}

TEST(OffsetEstimator, ChargesHalfTheStampResolution) {
    OffsetEstimator est;
    ASSERT_TRUE(
        est.add({.sent_ns = 100 * k_ms, .received_ns = 104 * k_ms, .server_ns = 99 * k_ms, .resolution_ns = k_ms}));
    const auto e = est.estimate();
    EXPECT_EQ(e.offset_ns, -static_cast<std::int64_t>(2'500'000));
    EXPECT_EQ(e.uncertainty_ns, 2'500'000U);
}

TEST(OffsetEstimator, RefusesABackwardsSample) {
    OffsetEstimator est;
    EXPECT_FALSE(est.add({.sent_ns = 10, .received_ns = 5, .server_ns = 7, .resolution_ns = 0}));
    EXPECT_EQ(est.estimate().method, ClockMethod::None);
}

TEST(OffsetEstimator, ForgetsOutsideTheWindow) {
    OffsetEstimator est;
    (void)est.add({.sent_ns = 0, .received_ns = k_ms, .server_ns = 1, .resolution_ns = 0});
    for (std::uint64_t i = 1; i <= OffsetEstimator::k_window; ++i) {
        (void)est.add({.sent_ns = i * k_s,
                       .received_ns = i * k_s + 20 * k_ms,
                       .server_ns = i * k_s + 10 * k_ms,
                       .resolution_ns = 0});
    }
    EXPECT_EQ(est.estimate().rtt_ns, 20 * k_ms);
    EXPECT_EQ(est.estimate().offset_ns, 0);
}

TEST(FeedBound, SmallestDifferenceBoundsTheOffset) {
    FeedBound bound;
    EXPECT_EQ(bound.estimate(1).method, ClockMethod::None);
    bound.add(static_cast<std::int64_t>(30 * k_ms));
    bound.add(static_cast<std::int64_t>(12 * k_ms));
    bound.rotate();
    bound.add(static_cast<std::int64_t>(20 * k_ms));
    auto e = bound.estimate(5);
    EXPECT_EQ(e.method, ClockMethod::FeedBound);
    EXPECT_EQ(e.offset_ns, -static_cast<std::int64_t>(12 * k_ms));
    bound.rotate();
    EXPECT_EQ(bound.estimate(5).offset_ns, -static_cast<std::int64_t>(20 * k_ms));
    bound.add(-static_cast<std::int64_t>(7200 * k_s));
    EXPECT_EQ(bound.estimate(5).offset_ns, -static_cast<std::int64_t>(20 * k_ms));
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

class ScriptedSource final : public ServerTimeSource {
public:
    std::deque<std::optional<OffsetSample>> answers;
    int reads{0};

    [[nodiscard]] std::string endpoint() const override { return "test/time"; }

    [[nodiscard]] std::optional<OffsetSample> read() override {
        ++reads;
        auto next = answers.front();
        answers.pop_front();
        return next;
    }
};

TEST(ProbeClock, BurstsThenPacesAndHonoursTheBudget) {
    ScriptedSource source;
    for (int i = 0; i < 4; ++i) {
        source.answers.push_back(OffsetSample{.sent_ns = 10, .received_ns = 30, .server_ns = 25, .resolution_ns = 0});
    }
    std::uint64_t now = 0;
    bool admit = false;
    ProbeClock probe{
        source,
        [&] { return now; },
        [&] { return admit; },
        [](std::uint64_t lo, std::uint64_t) { return lo; },
        ProbeSchedule{.burst = 2, .burst_gap_ns = 1 * k_s, .period_min_ns = 30 * k_s, .period_max_ns = 60 * k_s}};
    EXPECT_EQ(probe.tick(), 1 * k_s);
    EXPECT_EQ(source.reads, 0);
    admit = true;
    now = 1 * k_s;
    EXPECT_EQ(probe.tick(), 2 * k_s);
    now = 2 * k_s;
    EXPECT_EQ(probe.tick(), 32 * k_s);
    now = 10 * k_s;
    EXPECT_EQ(probe.tick(), 32 * k_s);
    EXPECT_EQ(source.reads, 2);
    EXPECT_EQ(probe.estimate().offset_ns, 5);
}

TEST(ServerTime, ParsesEachShape) {
    EXPECT_EQ(parse_server_time(R"({"serverTime":1700000000123})", "serverTime", k_ms), 1700000000123ULL * k_ms);
    EXPECT_EQ(parse_server_time(R"({"code":"0","data":[{"ts":"1700000000123"}],"msg":""})", "ts", k_ms),
              1700000000123ULL * k_ms);
    EXPECT_EQ(
        parse_server_time(R"({"result":{"timeSecond":"1700000000","timeNano":"1700000000123456789"}})", "timeNano", 1),
        1700000000123456789ULL);
    EXPECT_EQ(parse_server_time(R"({"status":"ok","tss":1,"ts":1700000000123})", "ts", k_ms), 1700000000123ULL * k_ms);
    EXPECT_EQ(parse_server_time("1700000000123", "", k_ms), 1700000000123ULL * k_ms);
    EXPECT_FALSE(parse_server_time(R"({"msg":"busy"})", "serverTime", k_ms).has_value());
    EXPECT_FALSE(parse_server_time(R"({"data":null})", "data", k_ms).has_value());
}

TEST(ServerTime, TableNamesMarketsBeforeTheDefault) {
    EXPECT_EQ(time_endpoint("binance", "perp")->host, "fapi.binance.com");
    EXPECT_EQ(time_endpoint("binance", "spot")->host, "api.binance.com");
    EXPECT_EQ(time_endpoint("mexc", "perp")->path, "/api/v1/contract/ping");
    EXPECT_FALSE(time_endpoint("hyperliquid", "perp").has_value());
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
