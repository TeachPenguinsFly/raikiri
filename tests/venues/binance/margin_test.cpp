#include <raikiri/venues/binance/envelope.hpp>
#include <raikiri/venues/binance/positions.hpp>
#include <raikiri/venues/binance/rest.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace raikiri::venues::binance {
namespace {

constexpr UnixNanos k_now{1'700'000'000'000'000'000ULL}; // 1700000000000 ms

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{RAIKIRI_BINANCE_FIXTURES} / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

void give_credentials(net::Signer& signer) {
    signer.set_api_key("test-api-key");
    ASSERT_TRUE(signer.secret().assign("test-secret").has_value());
}

TEST(BinanceMargin, LeverageIsASignedPostNamingTheSymbolAndTheFigure) {
    net::Signer signer;
    give_credentials(signer);
    RequestBuilder builder{signer};
    const auto request = builder.set_leverage("BTCUSDT", 20, k_now);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->method, net::HttpMethod::Post);
    EXPECT_EQ(builder.last_sign_message(), "symbol=BTCUSDT&leverage=20&recvWindow=5000&timestamp=1700000000000");
    EXPECT_TRUE(request->target.starts_with("/fapi/v1/leverage?symbol=BTCUSDT&leverage=20&")) << request->target;
    EXPECT_NE(request->target.find("&signature="), std::string_view::npos);
}

TEST(BinanceMargin, MarginTypeSpellsTheTwoModesAsTheVenueDoes) {
    net::Signer signer;
    give_credentials(signer);
    RequestBuilder builder{signer};
    const auto isolated = builder.set_margin_type("ETHUSDT", true, k_now);
    ASSERT_TRUE(isolated.has_value());
    EXPECT_EQ(isolated->method, net::HttpMethod::Post);
    EXPECT_TRUE(isolated->target.starts_with("/fapi/v1/marginType?")) << isolated->target;
    EXPECT_EQ(builder.last_sign_message(),
              "symbol=ETHUSDT&marginType=ISOLATED&recvWindow=5000&timestamp=1700000000000");
    ASSERT_TRUE(builder.set_margin_type("ETHUSDT", false, k_now).has_value());
    EXPECT_EQ(builder.last_sign_message(), "symbol=ETHUSDT&marginType=CROSSED&recvWindow=5000&timestamp=1700000000000");
}

TEST(BinanceMargin, SymbolConfigIsASignedReadOfOneSymbol) {
    net::Signer signer;
    give_credentials(signer);
    RequestBuilder builder{signer};
    const auto request = builder.symbol_config("BTCUSDT", k_now);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->method, net::HttpMethod::Get);
    EXPECT_TRUE(request->target.starts_with("/fapi/v1/symbolConfig?symbol=BTCUSDT&recvWindow=5000&"))
        << request->target;
}

TEST(BinanceMargin, NothingIsBuiltWithoutCredentialsOrOffTheFuturesApi) {
    net::Signer none;
    RequestBuilder unsigned_builder{none};
    EXPECT_EQ(unsigned_builder.set_leverage("BTCUSDT", 20, k_now).error(), BuildError::NoCredentials);
    EXPECT_EQ(unsigned_builder.set_margin_type("BTCUSDT", true, k_now).error(), BuildError::NoCredentials);
    EXPECT_EQ(unsigned_builder.symbol_config("BTCUSDT", k_now).error(), BuildError::NoCredentials);

    net::Signer signer;
    give_credentials(signer);
    RequestBuilder spot{signer, MarketType::Spot};
    EXPECT_EQ(spot.set_leverage("BTCUSDT", 20, k_now).error(), BuildError::BadValue);
    RequestBuilder perp{signer};
    EXPECT_EQ(perp.set_leverage("BTCUSDT", 0, k_now).error(), BuildError::BadValue);
    EXPECT_EQ(perp.set_leverage("", 20, k_now).error(), BuildError::BadValue);
}

TEST(BinanceMargin, SymbolConfigYieldsTheNamedSymbolsRow) {
    const auto body = fixture("rest_symbol_config.json");
    const auto eth = parse_symbol_config(body, "ETHUSDT");
    ASSERT_TRUE(eth.has_value());
    EXPECT_EQ(eth->margin_type, "ISOLATED");
    EXPECT_EQ(eth->leverage, 5);
    const auto btc = parse_symbol_config(body, "BTCUSDT");
    ASSERT_TRUE(btc.has_value());
    EXPECT_EQ(btc->margin_type, "CROSSED");
    EXPECT_EQ(btc->leverage, 21);
    EXPECT_FALSE(parse_symbol_config(body, "XRPUSDT").has_value());
    EXPECT_FALSE(parse_symbol_config("[]", "BTCUSDT").has_value());
}

TEST(BinanceMargin, TheMarginTypeSuccessBodyReadsAsCode200) {
    // a positive code is no error to the reader, so the margin type's {"code":200} reads as success
    const auto done = parse_rest_error(fixture("rest_margin_type.json"));
    ASSERT_TRUE(done.has_value());
    EXPECT_EQ(done->code, k_ok);
    const auto leverage = parse_rest_error(fixture("rest_leverage.json"));
    EXPECT_TRUE(!leverage.has_value() || leverage->code == k_ok);
}

} // namespace
} // namespace raikiri::venues::binance
