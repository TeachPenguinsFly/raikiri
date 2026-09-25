#include <raikiri/venues/binance/rest.hpp>

#include <gtest/gtest.h>

#include <string>

namespace raikiri::venues::binance {
namespace {

// binance's documented signing example
constexpr std::string_view k_doc_secret = "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j";
constexpr std::string_view
    k_doc_query = "symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC&quantity=1&price=0.1&recvWindow=5000&"
                  "timestamp=1499827319559";
constexpr std::string_view k_doc_signature = "c8db56825ae71d6d79447849e617115f4a920fa2acdcab2b053c4b2838bd6b71";

void give_credentials(net::Signer& signer, std::string_view secret = k_doc_secret) {
    signer.set_api_key("test-api-key");
    ASSERT_TRUE(signer.secret().assign(secret).has_value());
}

InstrumentBase btc_usdt() {
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Binance, MarketType::Perp, SymbolId{0});
    base.exchange_symbol = *SymbolText::from("BTCUSDT");
    base.tick_size = *parse_decimal_e9("0.10");
    base.lot_size = *parse_decimal_e9("0.001");
    return base;
}

OrderCmd limit_buy() {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.side = Side::Buy;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.instrument = InstrumentId::make(VenueId::Binance, MarketType::Perp, SymbolId{0});
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("01HQ8ZK9M4TESTULID000000AB");
    cmd.price = PriceTicks::from_raw(644'885); // 64488.50
    cmd.qty = QtyLots::from_raw(5);            // 0.005
    cmd.ts_created = UnixNanos{1'700'000'000'000'000'000ULL};
    return cmd;
}

// reads one parameter's value out of a built target
std::string param_of(std::string_view target, std::string_view key) {
    const std::string needle = std::string{key} + "=";
    auto pos = target.find(needle);
    if (pos == std::string_view::npos) {
        return {};
    }
    pos += needle.size();
    const auto end = target.find('&', pos);
    return std::string{target.substr(pos, end == std::string_view::npos ? end : end - pos)};
}

TEST(BinanceRest, SignatureMatchesTheDocumentedVector) {
    net::Signer signer;
    give_credentials(signer);
    std::array<char, net::k_hex_bytes> out{};
    const auto signature = signer.sign(k_doc_query, net::Signer::Encoding::Hex, out);
    EXPECT_EQ(signature, k_doc_signature);
}

TEST(BinanceRest, WithoutCredentialsEverySignedPathRefuses) {
    // an unsigned order is refused before the wire
    net::Signer signer;
    RequestBuilder builder{signer};
    const auto instrument = btc_usdt();
    const auto now = UnixNanos{1'700'000'000'000'000'000ULL};

    EXPECT_EQ(builder.create_order(limit_buy(), instrument, now).error(), BuildError::NoCredentials);
    EXPECT_EQ(builder.cancel_order(limit_buy(), instrument, now).error(), BuildError::NoCredentials);
    EXPECT_EQ(builder.open_orders(now).error(), BuildError::NoCredentials);
    EXPECT_EQ(builder.position_risk(now).error(), BuildError::NoCredentials);
    EXPECT_EQ(builder.account(now).error(), BuildError::NoCredentials);
    EXPECT_EQ(builder.listen_key_create().error(), BuildError::NoCredentials);
}

TEST(BinanceRest, CreateOrderSignsExactlyWhatItSends) {
    // the signature covers the query exactly as sent
    net::Signer signer;
    give_credentials(signer);
    RequestBuilder builder{signer};

    const auto request = builder.create_order(limit_buy(), btc_usdt(), UnixNanos{1'700'000'000'000'000'000ULL});
    ASSERT_TRUE(request.has_value());

    const std::string_view target = request->target;
    const auto qmark = target.find('?');
    ASSERT_NE(qmark, std::string_view::npos);
    const auto sig_pos = target.find("&signature=");
    ASSERT_NE(sig_pos, std::string_view::npos);

    const auto query_on_wire = target.substr(qmark + 1, sig_pos - qmark - 1);
    EXPECT_EQ(query_on_wire, builder.last_sign_message());

    std::array<char, net::k_hex_bytes> out{};
    const auto expected = signer.sign(query_on_wire, net::Signer::Encoding::Hex, out);
    EXPECT_EQ(target.substr(sig_pos + 11), expected);
}

TEST(BinanceRest, CreateOrderRendersPricesFromFixedPoint) {
    net::Signer signer;
    give_credentials(signer);
    RequestBuilder builder{signer};

    const auto request = builder.create_order(limit_buy(), btc_usdt(), UnixNanos{1'700'000'000'000'000'000ULL});
    ASSERT_TRUE(request.has_value());
    const std::string_view target = request->target;

    EXPECT_EQ(param_of(target, "symbol"), "BTCUSDT");
    EXPECT_EQ(param_of(target, "side"), "BUY");
    EXPECT_EQ(param_of(target, "type"), "LIMIT");
    EXPECT_EQ(param_of(target, "timeInForce"), "GTC");
    // no float on the path
    EXPECT_EQ(param_of(target, "price"), "64488.5");
    EXPECT_EQ(param_of(target, "quantity"), "0.005");
    EXPECT_EQ(param_of(target, "newClientOrderId"), "01HQ8ZK9M4TESTULID000000AB");
    EXPECT_EQ(param_of(target, "recvWindow"), "5000");
    EXPECT_EQ(param_of(target, "timestamp"), "1700000000000");
    EXPECT_EQ(request->method, net::HttpMethod::Post);
    ASSERT_EQ(request->headers.size(), 1U);
    EXPECT_EQ(request->headers[0].name, "X-MBX-APIKEY");
    // order parameters ride in the query even on post
    EXPECT_TRUE(request->body.empty());
}

TEST(BinanceRest, PostOnlyBecomesTheTimeInForce) {
    // post-only is tif GTX
    net::Signer signer;
    give_credentials(signer);
    RequestBuilder builder{signer};

    auto cmd = limit_buy();
    cmd.flags = OrderFlags::PostOnly;
    const auto request = builder.create_order(cmd, btc_usdt(), UnixNanos{1'700'000'000'000'000'000ULL});
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(param_of(request->target, "timeInForce"), "GTX");
}

TEST(BinanceRest, PostOnlyMarketOrderIsRefusedLocally) {
    net::Signer signer;
    give_credentials(signer);
    RequestBuilder builder{signer};

    auto cmd = limit_buy();
    cmd.order_type = OrderType::Market;
    cmd.flags = OrderFlags::PostOnly;
    const auto request = builder.create_order(cmd, btc_usdt(), UnixNanos{1'700'000'000'000'000'000ULL});
    ASSERT_FALSE(request.has_value());
    EXPECT_EQ(request.error(), BuildError::UnsupportedOrderType);
}

TEST(BinanceRest, MarketOrderCarriesNoPriceOrTimeInForce) {
    net::Signer signer;
    give_credentials(signer);
    RequestBuilder builder{signer};

    auto cmd = limit_buy();
    cmd.order_type = OrderType::Market;
    const auto request = builder.create_order(cmd, btc_usdt(), UnixNanos{1'700'000'000'000'000'000ULL});
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(param_of(request->target, "type"), "MARKET");
    EXPECT_TRUE(param_of(request->target, "price").empty());
    EXPECT_TRUE(param_of(request->target, "timeInForce").empty());
}

TEST(BinanceRest, CancelPrefersTheVenueIdAndFallsBackToOurs) {
    net::Signer signer;
    give_credentials(signer);
    RequestBuilder builder{signer};
    const auto now = UnixNanos{1'700'000'000'000'000'000ULL};

    auto cmd = limit_buy();
    cmd.kind = CmdKind::Cancel;
    auto request = builder.cancel_order(cmd, btc_usdt(), now);
    ASSERT_TRUE(request.has_value());
    // cancelling before the first ack uses the client id
    EXPECT_EQ(param_of(request->target, "origClientOrderId"), "01HQ8ZK9M4TESTULID000000AB");
    EXPECT_EQ(request->method, net::HttpMethod::Delete);

    cmd.target_exchange_order_id = *ExchangeOrderId::from("4611875134427365377");
    request = builder.cancel_order(cmd, btc_usdt(), now);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(param_of(request->target, "orderId"), "4611875134427365377");
    EXPECT_TRUE(param_of(request->target, "origClientOrderId").empty());
}

TEST(BinanceRest, AmendIsAPutCarryingBothNewValues) {
    net::Signer signer;
    give_credentials(signer);
    RequestBuilder builder{signer};

    auto cmd = limit_buy();
    cmd.kind = CmdKind::Amend;
    cmd.price = PriceTicks::from_raw(644'000);
    cmd.qty = QtyLots::from_raw(7);
    const auto request = builder.amend_order(cmd, btc_usdt(), UnixNanos{1'700'000'000'000'000'000ULL});
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->method, net::HttpMethod::Put);
    EXPECT_EQ(param_of(request->target, "price"), "64400");
    EXPECT_EQ(param_of(request->target, "quantity"), "0.007");
    // binance requires side on a modify
    EXPECT_EQ(param_of(request->target, "side"), "BUY");
}

TEST(BinanceRest, ListenKeyEndpointsAreNeverSigned) {
    // listen key calls use header auth only
    net::Signer signer;
    give_credentials(signer);
    RequestBuilder builder{signer};

    for (const auto& request :
         {builder.listen_key_create(), builder.listen_key_keepalive(), builder.listen_key_close()}) {
        ASSERT_TRUE(request.has_value());
        EXPECT_EQ(request->target, "/fapi/v1/listenKey");
        EXPECT_EQ(request->target.find("signature"), std::string_view::npos);
        ASSERT_EQ(request->headers.size(), 1U);
        EXPECT_EQ(request->headers[0].name, "X-MBX-APIKEY");
    }
}

TEST(BinanceRest, ReconciliationPullsAreSignedGets) {
    net::Signer signer;
    give_credentials(signer);
    RequestBuilder builder{signer};
    const auto now = UnixNanos{1'700'000'000'000'000'000ULL};

    const auto open = builder.open_orders(now);
    ASSERT_TRUE(open.has_value());
    EXPECT_EQ(open->method, net::HttpMethod::Get);
    EXPECT_NE(open->target.find("/fapi/v1/openOrders?"), std::string_view::npos);
    // account-wide so orders on unsubscribed instruments are found
    EXPECT_TRUE(param_of(open->target, "symbol").empty());

    const auto positions = builder.position_risk(now);
    ASSERT_TRUE(positions.has_value());
    EXPECT_NE(positions->target.find("/fapi/v3/positionRisk?"), std::string_view::npos);

    const auto query = builder.query_order("BTCUSDT", *ExchangeClientOrderId::from("01HQ8ZK9M4TESTULID000000AB"), now);
    ASSERT_TRUE(query.has_value());
    EXPECT_EQ(param_of(query->target, "origClientOrderId"), "01HQ8ZK9M4TESTULID000000AB");

    const auto trades = builder.user_trades("BTCUSDT", UnixNanos{1'699'000'000'000'000'000ULL}, now);
    ASSERT_TRUE(trades.has_value());
    EXPECT_EQ(param_of(trades->target, "startTime"), "1699000000000");

    const auto account = builder.account(now);
    ASSERT_TRUE(account.has_value());
    EXPECT_EQ(account->method, net::HttpMethod::Get);
    EXPECT_NE(account->target.find("/fapi/v2/account?"), std::string_view::npos);
    EXPECT_EQ(param_of(account->target, "timestamp"), "1700000000000");
    EXPECT_NE(account->target.find("&signature="), std::string_view::npos);
}

} // namespace

TEST(BinanceRest, TheIncomeTargetIsWhatTheVenueExpects) {
    net::Signer signer;
    give_credentials(signer);
    RequestBuilder builder{signer};
    const auto request = builder.funding_income(UnixNanos{0}, UnixNanos{1'700'000'000'000'000'000ULL});
    ASSERT_TRUE(request.has_value());
    EXPECT_TRUE(request->target.starts_with("/fapi/v1/income?"));
}

TEST(BinanceRest, DepthSnapshotPercentEncodesANonAsciiSymbol) {
    net::Signer signer;
    RequestBuilder builder{signer};
    EXPECT_EQ(builder.depth_snapshot("龙虾USDT", 100).target, "/fapi/v1/depth?symbol=%E9%BE%99%E8%99%BEUSDT&limit=100");
    EXPECT_EQ(builder.depth_snapshot("BTCUSDT", 100).target, "/fapi/v1/depth?symbol=BTCUSDT&limit=100");
}

} // namespace raikiri::venues::binance
