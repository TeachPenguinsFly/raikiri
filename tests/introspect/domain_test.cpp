#include <raikiri/introspect/queries.hpp>
#include <raikiri/introspect/stats.hpp>

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

namespace raikiri::introspect {
namespace {

constexpr std::uint64_t k_s = 1'000'000'000ULL;

GatewayStats gateway(std::string account, std::string venue, std::uint64_t taken_ns) {
    GatewayStats stats{};
    stats.account = std::move(account);
    stats.venue = std::move(venue);
    stats.taken_ns = taken_ns;
    stats.catalogue.instruments = 100;
    stats.subscriptions.push_back(SubscriptionStats{
        .channel = "bbo", .requested = 100, .accepted = 90, .refused = 10, .messages = 0, .per_second = 0});
    SocketStats pool{};
    pool.host = "ws";
    pool.sockets = 2;
    pool.open = 2;
    stats.sockets.push_back(pool);
    stats.rates.messages_per_second = 50;
    return stats;
}

TEST(IntrospectDomain, ASnapshotOlderThanTheLimitIsStaleAndADeadProcessIsDown) {
    EXPECT_EQ(classify(100 * k_s, true, 120 * k_s, 30 * k_s), Liveness::Up);
    EXPECT_EQ(classify(100 * k_s, true, 131 * k_s, 30 * k_s), Liveness::Stale);
    EXPECT_EQ(classify(100 * k_s, false, 101 * k_s, 30 * k_s), Liveness::Down);
    EXPECT_EQ(classify(200 * k_s, true, 100 * k_s, 30 * k_s), Liveness::Up);
}

TEST(IntrospectDomain, TheRollupSumsPerVenueAndLeavesADownGatewayOutOfTheTraffic) {
    const std::vector<GatewayView> views{
        {.stats = gateway("bybit", "bybit", 0), .state = Liveness::Up, .age_ms = 0},
        {.stats = gateway("bybit-spot", "bybit", 0), .state = Liveness::Stale, .age_ms = 0},
        {.stats = gateway("kucoin", "kucoin", 0), .state = Liveness::Down, .age_ms = 0},
    };
    const auto machine = rollup(views);
    ASSERT_EQ(machine.venues.size(), 2U);
    EXPECT_EQ(machine.venues[0].venue, "bybit");
    EXPECT_EQ(machine.venues[0].gateways, 2U);
    EXPECT_EQ(machine.venues[0].instruments, 200U);
    EXPECT_EQ(machine.venues[0].subscriptions, 180U);
    EXPECT_EQ(machine.venues[0].sockets, 4U);
    EXPECT_DOUBLE_EQ(machine.venues[0].messages_per_second, 100.0);
    EXPECT_EQ(machine.venues[1].down, 1U);
    EXPECT_EQ(machine.venues[1].instruments, 0U);
    EXPECT_EQ(machine.total.gateways, 3U);
    EXPECT_EQ(machine.total.instruments, 200U);
}

TEST(IntrospectDomain, TheRateIsTakenOverTheTrailingWindowOnly) {
    RateWindow window{60 * k_s};
    EXPECT_EQ(window.per_second(0), 0.0);
    RateWindow::Counts counts{};
    for (std::uint64_t t = 0; t <= 100; t += 5) {
        counts[0] = t <= 40 ? t * 1000 : 40'000 + (t - 40) * 10;
        window.push(t * k_s, counts);
    }
    EXPECT_NEAR(window.per_second(0), 10.0, 1e-9);
    EXPECT_NEAR(window.total_per_second(), 10.0, 1e-9);
}

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

TEST(IntrospectQueries, TheMachineViewClassifiesEachSnapshotAgainstTheInjectedClock) {
    FakeSource source;
    source.records["bybit"] = {.stats = gateway("bybit", "bybit", 995 * k_s), .process_alive = true};
    source.records["kucoin"] = {.stats = gateway("kucoin", "kucoin", 900 * k_s), .process_alive = true};
    source.records["pacifica"] = {.stats = gateway("pacifica", "pacifica", 999 * k_s), .process_alive = false};
    const GetMachineStats machine{source, [] { return 1000 * k_s; }};
    const auto stats = machine();
    EXPECT_EQ(stats.taken_ns, 1000 * k_s);
    ASSERT_EQ(stats.gateways.size(), 3U);
    EXPECT_EQ(stats.gateways[0].state, Liveness::Up);
    EXPECT_EQ(stats.gateways[0].age_ms, 5000U);
    EXPECT_EQ(stats.gateways[1].state, Liveness::Stale);
    EXPECT_EQ(stats.gateways[1].age_ms, 100'000U);
    EXPECT_EQ(stats.gateways[2].state, Liveness::Down);
    EXPECT_EQ(stats.rollup.total.gateways, 3U);
    EXPECT_EQ(stats.rollup.total.down, 1U);
}

TEST(IntrospectQueries, OneGatewayIsAnsweredByNameAndAnUnknownOneIsNothing) {
    FakeSource source;
    source.records["bybit"] = {.stats = gateway("bybit", "bybit", 10 * k_s), .process_alive = true};
    const GetGatewayStats one{source, [] { return 50 * k_s; }};
    const auto found = one("bybit");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->state, Liveness::Stale);
    EXPECT_FALSE(one("okx").has_value());
}

} // namespace
} // namespace raikiri::introspect
