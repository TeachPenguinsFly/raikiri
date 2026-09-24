#include <raikiri/config/rest_limits.hpp>
#include <raikiri/config/settings.hpp>

#include <gtest/gtest.h>

#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

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

// RAIKIRI_CONFIG_DIR is test-only; the library must not carry a build path
TEST(Settings, TheCommittedVenueFileLoads) {
    const auto file = load_venues(std::string{RAIKIRI_CONFIG_DIR} + "/venues.yaml");
    ASSERT_TRUE(file.has_value()) << file.error();
    ASSERT_TRUE(file->venues.contains("binance"));
    ASSERT_TRUE(file->venues.contains("bybit"));

    const auto& bin = file->venues.at("binance");
    ASSERT_EQ(rest_limits(bin).size(), 1U) << "binance meters one pool";
    EXPECT_EQ(bin.limits[0].window_ms, 60000U);
    EXPECT_LE(bin.limits[0].max, 2400U);
    EXPECT_GT(bin.limits[0].reserve, 0U) << "order entry must keep a floor";
    EXPECT_TRUE(bin.limits[0].match.empty());

    const auto& by = file->venues.at("bybit");
    ASSERT_EQ(rest_limits(by).size(), 3U) << "bybit meters each endpoint";
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
        EXPECT_TRUE(rest_limits(file->venues.at(name)).empty()) << name;
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

TEST(Settings, LighterHoldsOneSocketUnderTheIpsTwoHundredAMinute) {
    const auto file = load_venues(std::string{RAIKIRI_CONFIG_DIR} + "/venues.yaml");
    ASSERT_TRUE(file.has_value()) << file.error();
    const auto& lighter = file->venues.at("lighter");
    const auto* sockets = find_limit(lighter, "ws_sockets");
    const auto* minute = find_limit(lighter, "ws_messages", "ip");
    ASSERT_NE(sockets, nullptr);
    ASSERT_NE(minute, nullptr);
    EXPECT_EQ(sockets->max, 1U);
    EXPECT_LT(minute->max, 200U);
    EXPECT_EQ(minute->window_ms, 60000U);
}

TEST(Settings, EveryPooledVenueSizesItsSocketsFromTheTable) {
    const auto file = load_venues(std::string{RAIKIRI_CONFIG_DIR} + "/venues.yaml");
    ASSERT_TRUE(file.has_value()) << file.error();
    for (const auto& name : {"binance", "aster",   "bybit",  "gate",     "kucoin",    "xt",
                             "htx",     "weex",    "bingx",  "bitget",   "backpack",  "cryptocom",
                             "apex",    "blofin",  "grvt",   "edgex",    "dydx",      "phemex",
                             "coinbase", "pacifica", "okx",  "mexc",     "hyperliquid", "deribit",
                             "lighter"}) {
        ASSERT_TRUE(file->venues.contains(name)) << name;
        const auto& venue = file->venues.at(name);
        EXPECT_NE(find_limit(venue, "ws_streams"), nullptr) << name;
        EXPECT_NE(find_limit(venue, "ws_sockets"), nullptr) << name;
        EXPECT_NE(find_limit(venue, "ws_messages"), nullptr) << name;
    }
    const auto& bitfinex = file->venues.at("bitfinex");
    EXPECT_NE(find_limit(bitfinex, "ws_messages", "socket"), nullptr);
    EXPECT_EQ(find_limit(bitfinex, "ws_sockets"), nullptr) << "its pools hold one socket each";
    const auto& kraken = file->venues.at("kraken");
    ASSERT_NE(find_limit(kraken, "ws_streams"), nullptr);
    EXPECT_EQ(find_limit(kraken, "ws_streams")->max, 1024U);
}

TEST(Settings, TheMovedSocketNumbersKeepWhatWasMeasured) {
    const auto file = load_venues(std::string{RAIKIRI_CONFIG_DIR} + "/venues.yaml");
    ASSERT_TRUE(file.has_value()) << file.error();
    const auto max = [&](const char* venue, std::string_view kind, std::string_view scope = {}) {
        const auto* limit = find_limit(file->venues.at(venue), kind, scope);
        return limit == nullptr ? 0U : limit->max;
    };
    EXPECT_GE(max("weex", "ws_streams") * max("weex", "ws_sockets"), 1020U * 3U)
        << "the pool covers weex's catalogue";
    EXPECT_LT(max("weex", "ws_sockets"), 20U) << "and stays under its 20 sockets an IP";
    EXPECT_LE(max("edgex", "ws_streams"), 3601U) << "what one socket carried without a refusal";
    EXPECT_GE(max("edgex", "ws_streams") * max("edgex", "ws_sockets"), 2U * 180U + 1U);
    EXPECT_EQ(max("pacifica", "ws_streams"), 20U);
    EXPECT_LE(max("phemex", "ws_streams"), 100U);
    EXPECT_EQ(max("dydx", "ws_streams"), 32U);
    EXPECT_EQ(max("kucoin", "ws_streams"), 100U) << "past it kucoin drops in silence";
    const auto* coinbase = find_limit(file->venues.at("coinbase"), "ws_messages", "socket");
    ASSERT_NE(coinbase, nullptr);
    EXPECT_EQ(coinbase->window_ms, 150U * max("coinbase", "ws_sockets"))
        << "eight a second per IP, split over its sockets";
}

TEST(Settings, AFrameLimitIsACountOfSubscriptions) {
    const auto file = load_venues(std::string{RAIKIRI_CONFIG_DIR} + "/venues.yaml");
    ASSERT_TRUE(file.has_value()) << file.error();
    const std::array<std::pair<const char*, std::uint32_t>, 14> frames{
        {{"aster", 32U}, {"deribit", 256U}, {"okx", 64U}, {"kraken", 64U}, {"grvt", 50U},
         {"binance", 32U}, {"bybit", 10U}, {"gate", 64U}, {"xt", 32U}, {"bitget", 50U},
         {"backpack", 50U}, {"weex", 25U}, {"coinbase", 64U}, {"apex", 4U}}};
    for (const auto& [venue, per_frame] : frames) {
        const auto* frame = find_limit(file->venues.at(venue), "ws_frame");
        ASSERT_NE(frame, nullptr) << venue;
        EXPECT_EQ(frame->max, per_frame) << venue;
    }
    const auto load = [](std::string_view limit) {
        return load_venues(write_temp(std::string{"venues:\n  okx:\n    limits:\n      - "} +
                                      std::string{limit} + "\n"));
    };
    EXPECT_TRUE(load("{ name: f, kind: ws_frame, max: 64 }").has_value());
    EXPECT_FALSE(load("{ name: f, kind: ws_frame, max: 64, window_ms: 1000 }").has_value());
    EXPECT_FALSE(load("{ name: f, kind: ws_frame, max: 64, scope: ip }").has_value());
}

TEST(Settings, HyperliquidCapsChannelsPerIpNotOnlyPerSocket) {
    const auto file = load_venues(std::string{RAIKIRI_CONFIG_DIR} + "/venues.yaml");
    ASSERT_TRUE(file.has_value()) << file.error();
    const auto& hl = file->venues.at("hyperliquid");
    ASSERT_NE(find_limit(hl, "ws_streams"), nullptr);
    EXPECT_EQ(find_limit(hl, "ws_streams")->max, 1000U);
    ASSERT_NE(find_limit(hl, "ws_channels"), nullptr) << "a second socket is refused past 1000";
    EXPECT_EQ(find_limit(hl, "ws_channels")->max, 1000U);
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
