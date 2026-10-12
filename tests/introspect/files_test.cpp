#include <raikiri/introspect/files.hpp>

#include <gtest/gtest.h>

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <glaze/glaze.hpp>
#include <iterator>
#include <string>

namespace raikiri::introspect {
namespace {

constexpr std::uint64_t k_s = 1'000'000'000ULL;

class StatsFiles : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("raikiri-stats-" + std::to_string(::getpid()) + "-" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(dir_);
    }

    void TearDown() override { std::filesystem::remove_all(dir_); }

    [[nodiscard]] std::string dir() const { return dir_.string(); }

    static GatewayStats stats(const std::string& account, std::uint64_t taken_ns) {
        GatewayStats out{};
        out.account = account;
        out.venue = account;
        out.market = "perp";
        out.feed = "uta";
        out.channels = {"bbo", "depth"};
        out.taken_ns = taken_ns;
        out.process.pid = ::getpid();
        out.catalogue.instruments = 7;
        out.catalogue.by_asset_class = {{.name = "Crypto", .count = 7}};
        return out;
    }

private:
    std::filesystem::path dir_;
};

TEST_F(StatsFiles, APublishedSnapshotIsRenamedIntoPlaceAndCarriesTheSchema) {
    FileStatsPublisher publisher{dir()};
    ASSERT_TRUE(publisher.publish(stats("kucoin", 5 * k_s)));
    ASSERT_TRUE(publisher.publish(stats("kucoin", 10 * k_s)));

    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator{dir()}) {
        EXPECT_EQ(entry.path().filename(), "kucoin.json");
        ++files;
    }
    EXPECT_EQ(files, 1U);

    std::ifstream in{dir() + "/kucoin.json"};
    const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    EXPECT_EQ(text.size(), publisher.last_bytes());
    glz::generic json{};
    ASSERT_FALSE(glz::read_json(json, text));
    for (const auto* key : {"version",
                            "taken_ns",
                            "account",
                            "venue",
                            "market",
                            "feed",
                            "channels",
                            "process",
                            "catalogue",
                            "subscriptions",
                            "sockets",
                            "limits",
                            "coverage",
                            "rates"}) {
        EXPECT_TRUE(json.contains(key)) << key;
    }
    EXPECT_EQ(json["version"].get<double>(), k_stats_version);
    EXPECT_EQ(json["taken_ns"].get<std::string>(), std::to_string(10 * k_s));
}

TEST_F(StatsFiles, AnAccountThatWouldEscapeTheDirectoryIsRefused) {
    FileStatsPublisher publisher{dir()};
    EXPECT_FALSE(publisher.publish(stats("../x", 1)));
    EXPECT_FALSE(FileStatsSource{dir()}.read("../x").has_value());
}

TEST_F(StatsFiles, TwoSnapshotsAggregateAndTheOldOneIsMarkedStale) {
    FileStatsPublisher publisher{dir()};
    std::uint64_t now = 1000 * k_s;
    ASSERT_TRUE(publisher.publish(stats("bybit", now - 2 * k_s)));
    ASSERT_TRUE(publisher.publish(stats("kucoin", now - 45 * k_s)));
    std::ofstream{dir() + "/broken.json"} << "{not json";

    const FileStatsSource source{dir()};
    const GetMachineStats machine{source, [now] { return now; }};
    const auto result = machine();
    ASSERT_EQ(result.gateways.size(), 2U);
    EXPECT_EQ(result.gateways[0].stats.account, "bybit");
    EXPECT_EQ(result.gateways[0].state, Liveness::Up);
    EXPECT_EQ(result.gateways[1].stats.account, "kucoin");
    EXPECT_EQ(result.gateways[1].state, Liveness::Stale);
    EXPECT_EQ(result.gateways[1].age_ms, 45'000U);
    EXPECT_EQ(result.rollup.total.instruments, 14U);
    EXPECT_EQ(result.gateways[1].stats.feed, "uta");
    ASSERT_EQ(result.gateways[1].stats.catalogue.by_asset_class.size(), 1U);
}

TEST_F(StatsFiles, ASnapshotWhoseProcessIsGoneReadsAsDown) {
    FileStatsPublisher publisher{dir()};
    auto gone = stats("okx", 1);
    gone.process.pid = 0x7fff'fff0;
    ASSERT_TRUE(publisher.publish(gone));
    const auto record = FileStatsSource{dir()}.read("okx");
    ASSERT_TRUE(record.has_value());
    EXPECT_FALSE(record->process_alive);
}

} // namespace
} // namespace raikiri::introspect
