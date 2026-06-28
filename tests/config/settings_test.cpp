#include <raikiri/config/rest_limits.hpp>
#include <raikiri/config/settings.hpp>

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace raikiri::config {
namespace {

std::string write_temp(std::string_view body) {
    std::string path =
        (std::filesystem::temp_directory_path() / "raikiri-settings-XXXXXX").string();
    const int fd = ::mkstemp(path.data());
    if (fd >= 0) {
        ::close(fd);
    }
    std::ofstream out{path};
    out << body;
    return path;
}

// RAIKIRI_CONFIG_DIR is a TEST-ONLY definition: the library must never carry a
// build machine's absolute path, so the test names the shipped files itself.
//
// The committed files are the contract. A build whose loader cannot read what
// ships beside it configures nothing, and the failure would surface as a venue
// running unlimited rather than as an error anybody sees.
TEST(Settings, TheCommittedVenueFileLoads) {
    const auto file = load_venues(std::string{RAIKIRI_CONFIG_DIR} + "/venues.yaml");
    ASSERT_TRUE(file.has_value()) << file.error();
    ASSERT_TRUE(file->venues.contains("binance"));
    ASSERT_TRUE(file->venues.contains("bybit"));

    const auto& bin = file->venues.at("binance");
    ASSERT_EQ(bin.limits.size(), 1U) << "binance meters one pool";
    EXPECT_EQ(bin.limits[0].window_ms, 60000U);
    EXPECT_LE(bin.limits[0].max, 2400U);
    EXPECT_GT(bin.limits[0].reserve, 0U) << "order entry must keep a floor";
    EXPECT_TRUE(bin.limits[0].match.empty());

    const auto& by = file->venues.at("bybit");
    ASSERT_EQ(by.limits.size(), 3U) << "bybit meters each endpoint";
    EXPECT_LT(by.limits[2].max, by.limits[1].max) << "the transaction log is metered lower";

    for (const auto& name : {"binance", "bybit", "gate"}) {
        const auto ms = file->venues.at(name).order_answer_timeout_ms;
        EXPECT_GT(ms, 5'000U) << name << ": shorter than the REST deadline is a second timeout";
    }
}

TEST(Settings, AnUnmeasuredVenueHasNoLimitsRatherThanInventedOnes) {
    const auto file = load_venues(std::string{RAIKIRI_CONFIG_DIR} + "/venues.yaml");
    ASSERT_TRUE(file.has_value()) << file.error();
    for (const auto& name : {"bingx", "bitget"}) {
        ASSERT_TRUE(file->venues.contains(name)) << name;
        EXPECT_TRUE(file->venues.at(name).limits.empty()) << name;
    }
}

TEST(Settings, TheCommittedCostsPriceAsterLikeTheVenueDoes) {
    const auto file = load_venues(std::string{RAIKIRI_CONFIG_DIR} + "/venues.yaml");
    ASSERT_TRUE(file.has_value()) << file.error();
    const auto& aster = file->venues.at("aster");
    const exec::RateBudget budget{rest_limits(aster), rest_costs(aster), aster.default_cost};
    EXPECT_EQ(budget.cost("/fapi/v1/premiumIndex"), 10U);
    EXPECT_EQ(budget.cost("/fapi/v1/fundingInfo"), 10U);
    EXPECT_EQ(budget.cost("/fapi/v1/exchangeInfo"), 1U);
    EXPECT_EQ(budget.cost("/fapi/v1/ticker/24hr"), 40U);
    EXPECT_EQ(budget.cost("/fapi/v1/depth?symbol=BTCUSDT&limit=5"), 2U);
    EXPECT_EQ(budget.cost("/fapi/v1/depth?symbol=BTCUSDT&limit=100"), 5U);
    EXPECT_EQ(budget.cost("/fapi/v1/depth?symbol=BTCUSDT&limit=500"), 10U);
    EXPECT_EQ(budget.cost("/fapi/v1/depth?symbol=BTCUSDT&limit=1000"), 20U);
    EXPECT_EQ(budget.cost("/fapi/v1/depth?symbol=BTCUSDT&limit=7"), 20U);
    EXPECT_EQ(budget.cost("/fapi/v1/depth?symbol=BTCUSDT"), 20U);
    EXPECT_EQ(budget.cost("/fapi/v1/somethingNobodyCosted"), 40U);
}

TEST(Settings, TheCommittedCostsWeighHyperliquidAndKraken) {
    const auto file = load_venues(std::string{RAIKIRI_CONFIG_DIR} + "/venues.yaml");
    ASSERT_TRUE(file.has_value()) << file.error();
    const auto& hl = file->venues.at("hyperliquid");
    const exec::RateBudget hyperliquid{rest_limits(hl), rest_costs(hl), hl.default_cost};
    EXPECT_EQ(hyperliquid.cost("/info"), 20U) << "meta weighs 20";
    EXPECT_EQ(hyperliquid.cost("/exchange"), 1U);
    const auto& kr = file->venues.at("kraken");
    const exec::RateBudget kraken{rest_limits(kr), rest_costs(kr), kr.default_cost};
    EXPECT_EQ(kraken.cost("/derivatives/api/v3/instruments"), 0U) << "public costs nothing";
    EXPECT_EQ(kraken.cost("/derivatives/api/v3/sendorder"), 10U);
    EXPECT_EQ(kraken.cost("/derivatives/api/v3/openpositions"), 2U);
}

TEST(Settings, ABadLimitIsRefusedNamingTheVenueAndTheLimit) {
    const auto load = [](std::string_view limit) {
        return load_venues(write_temp(std::string{"venues:\n  okx:\n    limits:\n      - "} +
                                      std::string{limit} + "\n"));
    };
    const auto zero_window = load("{ name: market, window_ms: 0, max: 18 }");
    ASSERT_FALSE(zero_window.has_value());
    EXPECT_NE(zero_window.error().find("okx limit 'market'"), std::string::npos)
        << zero_window.error();
    EXPECT_FALSE(load("{ name: m, window_ms: 1000, max: 5, reserve: 5 }").has_value());
    EXPECT_FALSE(load("{ name: m, kind: ws_bogus, max: 5 }").has_value());
    EXPECT_FALSE(load("{ name: m, kind: ws_streams, max: 5, window_ms: 1000 }").has_value());
    EXPECT_FALSE(load("{ name: m, window_ms: 1000, maximum: 5 }").has_value())
        << "an unknown key is refused, not ignored";
    EXPECT_TRUE(load("{ name: m, window_ms: 1000, max: 5 }").has_value());
}

// The DSN carries a password, and this file is committed: it must name where
// the secret lives and never hold one.
TEST(Settings, TheDatabaseFileNamesTheSecretRatherThanHoldingIt) {
    const auto file = load_database(std::string{RAIKIRI_CONFIG_DIR} + "/database.yaml");
    ASSERT_TRUE(file.has_value()) << file.error();
    const auto& dsn = file->database.dsn;
    ASSERT_FALSE(dsn.empty());
    const bool is_reference = dsn.starts_with("env:") || dsn.starts_with("file:") ||
                              dsn.starts_with("exec:") || dsn.starts_with("op://");
    EXPECT_TRUE(is_reference) << "a committed DSN must be a reference, got: " << dsn;
    EXPECT_EQ(dsn.find("password"), std::string::npos);
    EXPECT_EQ(dsn.find("://raikiri:"), std::string::npos) << "that is a literal credential";
}

TEST(Settings, AMissingFileSaysSoRatherThanReadingAsEmpty) {
    const auto file = load_venues("config/there-is-no-such-file.yaml");
    EXPECT_FALSE(file.has_value());
}

} // namespace
} // namespace raikiri::config
