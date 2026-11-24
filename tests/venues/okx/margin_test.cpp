#include <raikiri/venues/okx/envelope.hpp>
#include <raikiri/venues/okx/positions.hpp>
#include <raikiri/venues/okx/rest.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace raikiri::venues::okx {
namespace {

constexpr UnixNanos k_now{1'700'000'000'123'000'000ULL};

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{RAIKIRI_OKX_FIXTURES} / name;
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

TEST(OkxMargin, LeverageIsSetUnderTheModeTheOrdersUse) {
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder rest{signer};
    rest.set_passphrase("pass");
    const auto request = rest.set_leverage("BTC-USDT-SWAP", 5, "cross", k_now);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->method, net::HttpMethod::Post);
    EXPECT_NE(request->target.find("/api/v5/account/set-leverage"), std::string_view::npos) << request->target;
    for (const std::string_view field : {R"("instId":"BTC-USDT-SWAP")", R"("lever":"5")", R"("mgnMode":"cross")"}) {
        EXPECT_NE(request->body.find(field), std::string_view::npos) << field << " in " << request->body;
    }
    EXPECT_NE(rest.last_sign_message().find("/api/v5/account/set-leverage"), std::string_view::npos);
    EXPECT_NE(rest.last_sign_message().find(R"("lever":"5")"), std::string_view::npos);
}

TEST(OkxMargin, LeverageInfoNamesTheInstrumentAndTheMode) {
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder rest{signer};
    rest.set_passphrase("pass");
    const auto request = rest.leverage_info("BTC-USDT-SWAP", "cross", k_now);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->method, net::HttpMethod::Get);
    EXPECT_NE(request->target.find("/api/v5/account/leverage-info"), std::string_view::npos) << request->target;
    EXPECT_NE(request->target.find("instId=BTC-USDT-SWAP&mgnMode=cross"), std::string_view::npos) << request->target;
}

TEST(OkxMargin, LeverageInfoAnswersOneRowASideWithTheSameFigure) {
    const auto lever = parse_leverage_info(fixture("rest_leverage_info.json"));
    ASSERT_TRUE(lever.has_value());
    EXPECT_EQ(*lever, 10U);
}

TEST(OkxMargin, TheSetAnswerEchoesTheLeverage) {
    const auto body = fixture("rest_set_leverage.json");
    const auto envelope = parse_rest_envelope(body);
    ASSERT_TRUE(envelope.has_value());
    EXPECT_EQ(envelope->code, k_ok);
    const auto lever = parse_leverage_info(body);
    ASSERT_TRUE(lever.has_value());
    EXPECT_EQ(*lever, 30U);
}

TEST(OkxMargin, ARefusalIsACodeWithTheVenueReasonAndNoRows) {
    constexpr std::string_view k_refused =
        R"({"code":"59107","msg":"Leverage can't be modified. Please cancel all pending cross-margin orders before )"
        R"(adjusting the leverage.","data":[]})";
    const auto envelope = parse_rest_envelope(k_refused);
    ASSERT_TRUE(envelope.has_value());
    EXPECT_EQ(envelope->code, 59107);
    EXPECT_NE(envelope->msg.find("pending cross-margin orders"), std::string_view::npos);
    EXPECT_FALSE(parse_leverage_info(k_refused).has_value());
}

} // namespace
} // namespace raikiri::venues::okx
