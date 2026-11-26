#include <raikiri/venues/binance/terms.hpp>
#include <raikiri/venues/bybit/envelope.hpp>
#include <raikiri/venues/bybit/terms.hpp>
#include <raikiri/venues/hyperliquid/terms.hpp>
#include <raikiri/venues/okx/terms.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace raikiri::venues {
namespace {

std::string fixture(std::string_view venue, std::string_view name) {
    const auto path = std::filesystem::path{RAIKIRI_VENUE_FIXTURES} / venue / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

void expect_ascending(const std::vector<MaintenanceTier>& tiers) {
    for (std::size_t i = 1; i < tiers.size(); ++i) {
        EXPECT_GT(tiers[i].floor, tiers[i - 1].floor) << "tier " << i;
        EXPECT_GE(tiers[i].rate, tiers[i - 1].rate) << "tier " << i;
    }
}

// the fixture is shaped after the venue's documentation, not captured: the call needs a key
TEST(BinanceTerms, TheCommissionRateIsTheAccountsOwn) {
    const auto rate = binance::parse_commission_rate(fixture("binance", "rest_commission_rate.json"));
    ASSERT_TRUE(rate.has_value());
    EXPECT_DOUBLE_EQ(rate->maker, 0.0002);
    EXPECT_DOUBLE_EQ(rate->taker, 0.0004);
}

TEST(BinanceTerms, BracketsCarryTheirCumulativeAmount) {
    const auto body = fixture("binance", "rest_leverage_bracket.json");
    const auto tiers = binance::parse_leverage_brackets(body, "ETHUSDT");
    ASSERT_TRUE(tiers.has_value());
    ASSERT_EQ(tiers->size(), 3U);
    EXPECT_EQ((*tiers)[1],
              (MaintenanceTier{.floor = 10'000, .cap = 50'000, .rate = 0.01, .amount = 35, .max_leverage = 50}));
    EXPECT_DOUBLE_EQ(*maintenance_margin(*tiers, 20'000), 165.0);
    EXPECT_FALSE(binance::parse_leverage_brackets(body, "BTCUSDT").has_value());
}

TEST(BinanceTerms, ASingleSymbolMayAnswerAsOneObject) {
    const std::string body =
        R"({"symbol":"ETHUSDT","brackets":[{"bracket":1,"initialLeverage":75,"notionalCap":10000,"notionalFloor":0,"maintMarginRatio":0.0065,"cum":0}]})";
    const auto tiers = binance::parse_leverage_brackets(body, "ETHUSDT");
    ASSERT_TRUE(tiers.has_value());
    EXPECT_EQ(tiers->size(), 1U);
}

TEST(BybitTerms, AFeeRateRowIsReadByItsSymbol) {
    const auto body = fixture("bybit", "rest_fee_rate.json");
    const auto envelope = bybit::parse_envelope(body);
    ASSERT_TRUE(envelope.has_value());
    const auto rate = bybit::parse_fee_rate(envelope->result, "ETHUSDT");
    ASSERT_TRUE(rate.has_value());
    EXPECT_DOUBLE_EQ(rate->maker, 0.0001);
    EXPECT_DOUBLE_EQ(rate->taker, 0.0006);
    EXPECT_FALSE(bybit::parse_fee_rate(envelope->result, "BTCUSDT").has_value());
}

TEST(BybitTerms, RiskLimitTiersChainFloorToCapAndStayContinuous) {
    const auto body = fixture("bybit", "rest_risk_limit.json");
    const auto envelope = bybit::parse_envelope(body);
    ASSERT_TRUE(envelope.has_value());
    const auto tiers = bybit::parse_risk_limit(envelope->result, "BTCUSDT");
    ASSERT_TRUE(tiers.has_value());
    ASSERT_EQ(tiers->size(), 35U);
    EXPECT_EQ((*tiers)[0],
              (MaintenanceTier{.floor = 0, .cap = 300'000, .rate = 0.0033, .amount = 0, .max_leverage = 150}));
    EXPECT_EQ((*tiers)[1],
              (MaintenanceTier{.floor = 300'000, .cap = 2'000'000, .rate = 0.005, .amount = 510, .max_leverage = 100}));
    expect_ascending(*tiers);
    // the venue's own deductions make the margin meet at each floor
    EXPECT_NEAR(*maintenance_margin(*tiers, 299'999.99), *maintenance_margin(*tiers, 300'000), 0.01);
    EXPECT_NEAR(*maintenance_margin(*tiers, 1'999'999.99), *maintenance_margin(*tiers, 2'000'000), 0.01);
    EXPECT_TRUE(bybit::parse_risk_limit(envelope->result, "ETHUSDT")->empty());
}

// shaped after the documentation; okx writes a charge as a negative rate
TEST(OkxTerms, AChargeComesOutPositiveAndAnEmptyPairIsAbsent) {
    const auto fee = okx::parse_trade_fee(fixture("okx", "rest_trade_fee.json"));
    ASSERT_TRUE(fee.has_value());
    ASSERT_TRUE(fee->usdt.has_value());
    EXPECT_DOUBLE_EQ(fee->usdt->maker, 0.0002);
    EXPECT_DOUBLE_EQ(fee->usdt->taker, 0.0005);
    EXPECT_TRUE(fee->coin.has_value());
    EXPECT_FALSE(fee->usdc.has_value());
}

TEST(OkxTerms, ARebateComesOutNegative) {
    const auto fee = okx::parse_trade_fee(
        R"({"code":"0","data":[{"maker":"0.00005","taker":"-0.0003","makerU":"","takerU":""}],"msg":""})");
    ASSERT_TRUE(fee.has_value());
    ASSERT_TRUE(fee->coin.has_value());
    EXPECT_DOUBLE_EQ(fee->coin->maker, -0.00005);
    EXPECT_DOUBLE_EQ(fee->coin->taker, 0.0003);
    EXPECT_FALSE(fee->usdt.has_value());
}

TEST(OkxTerms, PositionTiersAreInContractsWithNoAmount) {
    const auto tiers = okx::parse_position_tiers(fixture("okx", "rest_position_tiers.json"));
    ASSERT_TRUE(tiers.has_value());
    ASSERT_EQ(tiers->size(), 99U);
    EXPECT_EQ((*tiers)[0],
              (MaintenanceTier{.floor = 0, .cap = 1'000, .rate = 0.004, .amount = 0, .max_leverage = 100}));
    EXPECT_DOUBLE_EQ((*tiers)[1].floor, 1'000.01);
    EXPECT_DOUBLE_EQ((*tiers)[1].max_leverage, 66.66);
    expect_ascending(*tiers);
}

TEST(OkxTerms, TheFamilyIsTheFirstTwoParts) {
    EXPECT_EQ(okx::inst_family_of("BTC-USDT-SWAP"), "BTC-USDT");
    EXPECT_EQ(okx::inst_family_of("BTC-USD-251226"), "BTC-USD");
    EXPECT_EQ(okx::inst_family_of("BTC-USD-251226-90000-C"), "BTC-USD");
    EXPECT_EQ(okx::inst_family_of("BTC-USDT"), "");
}

TEST(HyperliquidTerms, ATableGivesHalfTheInitialRateAndAContinuousAmount) {
    const auto meta = fixture("hyperliquid", "rest_meta_margin_tables.json");
    const auto tiers = hyperliquid::parse_margin_tiers(meta, "BTC");
    ASSERT_TRUE(tiers.has_value());
    ASSERT_EQ(tiers->size(), 2U);
    EXPECT_EQ((*tiers)[0],
              (MaintenanceTier{.floor = 0, .cap = 150'000'000, .rate = 0.0125, .amount = 0, .max_leverage = 40}));
    EXPECT_EQ(
        (*tiers)[1],
        (MaintenanceTier{.floor = 150'000'000, .cap = 0, .rate = 0.025, .amount = 1'875'000, .max_leverage = 20}));
}

TEST(HyperliquidTerms, AnIdUnderFiftyIsOneTierAtThatLeverage) {
    const auto meta = fixture("hyperliquid", "rest_meta_margin_tables.json");
    const auto tiers = hyperliquid::parse_margin_tiers(meta, "PURR");
    ASSERT_TRUE(tiers.has_value());
    ASSERT_EQ(tiers->size(), 1U);
    EXPECT_DOUBLE_EQ((*tiers)[0].rate, 1.0 / 6.0);
    EXPECT_DOUBLE_EQ((*tiers)[0].max_leverage, 3.0);
    EXPECT_FALSE(hyperliquid::parse_margin_tiers(meta, "NOSUCHCOIN").has_value());
}

TEST(HyperliquidTerms, ADexCoinIsFoundUnderItsPrefixedName) {
    const auto tiers = hyperliquid::parse_margin_tiers(fixture("hyperliquid", "rest_meta_dex_xyz.json"), "xyz:XYZ100");
    ASSERT_TRUE(tiers.has_value());
    ASSERT_EQ(tiers->size(), 1U);
    EXPECT_DOUBLE_EQ((*tiers)[0].max_leverage, 30.0);
}

} // namespace
} // namespace raikiri::venues
