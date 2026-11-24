#include <raikiri/venues/bybit/account.hpp>
#include <raikiri/venues/bybit/envelope.hpp>
#include <raikiri/venues/bybit/positions.hpp>
#include <raikiri/venues/bybit/rest.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace raikiri::venues::bybit {
namespace {

constexpr UnixNanos k_now{1'700'000'000'123'000'000ULL};

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{RAIKIRI_BYBIT_FIXTURES} / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

void load_credentials(net::Signer& signer) {
    signer.set_api_key("TESTKEY123");
    ASSERT_TRUE(signer.secret().assign("testsecret").has_value());
}

TEST(BybitMargin, LeverageGoesToBothSidesOfALinearSymbol) {
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder builder{signer};
    const auto request = builder.set_leverage(MarketType::Perp, "BTCUSDT", 6, k_now);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->method, net::HttpMethod::Post);
    EXPECT_NE(request->target.find("/v5/position/set-leverage"), std::string_view::npos) << request->target;
    for (const std::string_view field :
         {R"("category":"linear")", R"("symbol":"BTCUSDT")", R"("buyLeverage":"6")", R"("sellLeverage":"6")"}) {
        EXPECT_NE(request->body.find(field), std::string_view::npos) << field << " in " << request->body;
    }
    // the body is what is signed
    EXPECT_NE(builder.last_sign_message().find(R"("buyLeverage":"6")"), std::string_view::npos);
}

TEST(BybitMargin, TheLeverageIsReadFromOneSymbolsPositionRow) {
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder builder{signer};
    const auto request = builder.position(MarketType::Perp, "BTCUSDT", k_now);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->method, net::HttpMethod::Get);
    EXPECT_NE(request->target.find("/v5/position/list"), std::string_view::npos) << request->target;
    EXPECT_NE(request->target.find("category=linear&symbol=BTCUSDT"), std::string_view::npos) << request->target;
}

TEST(BybitMargin, WithoutCredentialsNothingIsBuilt) {
    net::Signer signer;
    RequestBuilder builder{signer};
    EXPECT_FALSE(builder.set_leverage(MarketType::Perp, "BTCUSDT", 6, k_now).has_value());
    EXPECT_FALSE(builder.position(MarketType::Perp, "BTCUSDT", k_now).has_value());
    EXPECT_FALSE(builder.set_margin_mode("ISOLATED_MARGIN", k_now).has_value());
}

TEST(BybitMargin, AFlatSymbolsRowStillCarriesItsLeverage) {
    const auto leverage = parse_position_leverage(fixture("rest_position_one.json"));
    ASSERT_TRUE(leverage.has_value());
    EXPECT_EQ(*leverage, 10U);
}

TEST(BybitMargin, AnEmptyLeverageIsUnknownAndAnEmptyListIsAnError) {
    // portfolio margin answers ""
    const auto unknown = parse_position_leverage(
        R"({"list":[{"symbol":"BTCUSDT","leverage":""}],"category":"linear"})");
    ASSERT_TRUE(unknown.has_value());
    EXPECT_EQ(*unknown, 0U);
    const auto fractional = parse_position_leverage(R"({"list":[{"symbol":"BTCUSDT","leverage":"12.5"}]})");
    ASSERT_TRUE(fractional.has_value());
    EXPECT_EQ(*fractional, 12U);
    EXPECT_FALSE(parse_position_leverage(R"({"list":[],"category":"linear"})").has_value());
}

TEST(BybitMargin, TheMarginModeIsSetForTheWholeAccount) {
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder builder{signer};
    const auto request = builder.set_margin_mode("ISOLATED_MARGIN", k_now);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->method, net::HttpMethod::Post);
    EXPECT_NE(request->target.find("/v5/account/set-margin-mode"), std::string_view::npos) << request->target;
    EXPECT_EQ(request->body, R"({"setMarginMode":"ISOLATED_MARGIN"})");
    EXPECT_NE(builder.last_sign_message().find(R"({"setMarginMode":"ISOLATED_MARGIN"})"), std::string_view::npos);
}

TEST(BybitMargin, AnAcceptedSwitchHasNoReasons) {
    const auto body = fixture("rest_set_margin_mode.json");
    const auto envelope = parse_envelope(body);
    ASSERT_TRUE(envelope.has_value());
    EXPECT_EQ(envelope->ret_code, 0);
    const auto refusal = parse_margin_mode_refusal(envelope->result);
    ASSERT_TRUE(refusal.has_value());
    EXPECT_TRUE(refusal->message.empty());
}

TEST(BybitMargin, ARefusedSwitchCarriesTheVenueReason) {
    const auto body = fixture("rest_set_margin_mode_refused.json");
    const auto envelope = parse_envelope(body);
    ASSERT_TRUE(envelope.has_value());
    EXPECT_EQ(envelope->ret_code, 3400045);
    EXPECT_EQ(classify(envelope->ret_code), exec::ErrorClass::Terminal);
    const auto refusal = parse_margin_mode_refusal(envelope->result);
    ASSERT_TRUE(refusal.has_value());
    EXPECT_EQ(refusal->code, "3400000");
    EXPECT_EQ(refusal->message, "Equity needs to be equal to or greater than 1000 USDC");
}

TEST(BybitMargin, TheAccountModeIsReadFromAccountInfo) {
    const auto mode = parse_margin_mode(R"({"marginMode":"REGULAR_MARGIN","updatedTime":"1697078946000",)"
                                        R"("unifiedMarginStatus":4,"dcpStatus":"OFF","timeWindow":10,"smpGroup":0,)"
                                        R"("isMasterTrader":false,"spotHedgingStatus":"OFF"})");
    ASSERT_TRUE(mode.has_value());
    EXPECT_EQ(*mode, "REGULAR_MARGIN");
}

} // namespace
} // namespace raikiri::venues::bybit
