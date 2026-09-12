#include <raikiri/introspect/routes.hpp>

#include <gtest/gtest.h>

#include <glaze/glaze.hpp>

#include <map>
#include <string>
#include <vector>

namespace raikiri::introspect {
namespace {

constexpr std::uint64_t k_s = 1'000'000'000ULL;

class FakeSource final : public GatewayStatsSource {
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
        if (it == records.end()) {
            return std::nullopt;
        }
        return it->second;
    }
};

http::Request get(std::string_view path) {
    http::Request req{};
    req.method = "GET";
    req.path = path;
    return req;
}

class CacheRoutesTest : public ::testing::Test {
protected:
    void SetUp() override {
        GatewayStats fresh{};
        fresh.account = "bybit";
        fresh.venue = "bybit";
        fresh.taken_ns = 95 * k_s;
        fresh.catalogue.instruments = 500;
        GatewayStats old = fresh;
        old.account = "bybit-spot";
        old.taken_ns = 10 * k_s;
        source_.records["bybit"] = {.stats = fresh, .process_alive = true};
        source_.records["bybit-spot"] = {.stats = old, .process_alive = true};
    }

    FakeSource source_;
    GetMachineStats machine_{source_, [] { return 100 * k_s; }};
    GetGatewayStats gateway_{source_, [] { return 100 * k_s; }};
    CacheRoutes routes_{machine_, gateway_};
};

TEST_F(CacheRoutesTest, TheMachineViewAnswersJsonWithEveryGatewayAndTheRollup) {
    const auto answer = routes_(get("/cache/stats"));
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(answer->status, 200);
    EXPECT_EQ(answer->content_type, "application/json");
    glz::generic json{};
    ASSERT_FALSE(glz::read_json(json, answer->body));
    EXPECT_EQ(json["taken_ns"].get<std::string>(), std::to_string(100 * k_s));
    auto& gateways = json["gateways"].get<glz::generic::array_t>();
    ASSERT_EQ(gateways.size(), 2U);
    EXPECT_EQ(gateways[0]["state"].get<std::string>(), "up");
    EXPECT_FALSE(gateways[0]["stale"].get<bool>());
    EXPECT_EQ(gateways[1]["account"].get<std::string>(), "bybit-spot");
    EXPECT_TRUE(gateways[1]["stale"].get<bool>());
    EXPECT_EQ(gateways[1]["age_ms"].get<double>(), 90'000.0);
    EXPECT_EQ(gateways[1]["snapshot"]["catalogue"]["instruments"].get<double>(), 500.0);
    EXPECT_EQ(json["rollup"]["total"]["instruments"].get<double>(), 1000.0);
    EXPECT_EQ(json["rollup"]["venues"][0]["venue"].get<std::string>(), "bybit");
}

TEST_F(CacheRoutesTest, OneAccountIsServedByNameAndAnUnknownOneIsA404) {
    const auto one = routes_(get("/cache/stats/bybit"));
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(one->status, 200);
    glz::generic json{};
    ASSERT_FALSE(glz::read_json(json, one->body));
    EXPECT_EQ(json["account"].get<std::string>(), "bybit");

    const auto missing = routes_(get("/cache/stats/okx"));
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(missing->status, 404);
    EXPECT_EQ(routes_(get("/cache/nothing"))->status, 404);
}

TEST_F(CacheRoutesTest, APathOutsideTheNamespaceFallsThroughAndAWriteIsRefused) {
    EXPECT_FALSE(routes_(get("/v1/health")).has_value());
    auto post = get("/cache/stats");
    post.method = "POST";
    EXPECT_EQ(routes_(post)->status, 405);
}

} // namespace
} // namespace raikiri::introspect
