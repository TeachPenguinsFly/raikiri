#include <raikiri/venues/binance/rest.hpp>
#include <raikiri/venues/binance/ws_api.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::binance {
namespace {

constexpr std::string_view k_secret = "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j";
constexpr UnixNanos k_now{1'700'000'000'000'000'000ULL};

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{RAIKIRI_BINANCE_FIXTURES} / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// a doc example under one of our numeric ids
std::string with_id(std::string frame, std::string_view id) {
    const auto start = frame.find(R"("id":")") + 6;
    const auto end = frame.find('"', start);
    frame.replace(start, end - start, id);
    return frame;
}

void give_credentials(net::Signer& signer) {
    signer.set_api_key("test-api-key");
    ASSERT_TRUE(signer.secret().assign(k_secret).has_value());
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
    cmd.flags = OrderFlags::PostOnly;
    cmd.instrument = btc_usdt().id;
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("01HQ8ZK9M4TESTULID000000AB");
    cmd.price = PriceTicks::from_raw(644'885);
    cmd.qty = QtyLots::from_raw(5);
    return cmd;
}

TEST(BinanceWsApi, PlaceCarriesTheRestQuerySortedWithTheKeyAndSignsThat) {
    net::Signer signer;
    give_credentials(signer);
    RequestBuilder rest{signer};
    WsApiBuilder ws{signer};
    ASSERT_TRUE(rest.create_order(limit_buy(), btc_usdt(), k_now).has_value());

    const auto frame = ws.request(9, k_ws_order_place, rest.last_sign_message());
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(ws.last_sign_message(),
              "apiKey=test-api-key&newClientOrderId=01HQ8ZK9M4TESTULID000000AB&price=64488.5&quantity=0.005"
              "&recvWindow=5000&side=BUY&symbol=BTCUSDT&timeInForce=GTX&timestamp=1700000000000&type=LIMIT");

    std::array<char, net::k_hex_bytes> expected{};
    const auto signature = net::hmac_sha256_hex(k_secret, ws.last_sign_message(), expected);
    EXPECT_EQ(*frame,
              R"({"id":"9","method":"order.place","params":{"apiKey":"test-api-key",)"
              R"("newClientOrderId":"01HQ8ZK9M4TESTULID000000AB","price":"64488.5","quantity":"0.005",)"
              R"("recvWindow":5000,"side":"BUY","symbol":"BTCUSDT","timeInForce":"GTX","timestamp":1700000000000,)"
              R"("type":"LIMIT","signature":")" +
                  std::string{signature} + R"("}})");
}

TEST(BinanceWsApi, CancelAndModifyKeepTheOrderIdAsAnInteger) {
    net::Signer signer;
    give_credentials(signer);
    RequestBuilder rest{signer};
    WsApiBuilder ws{signer};
    auto cmd = limit_buy();
    cmd.target_exchange_order_id = *ExchangeOrderId::from("336829446");

    ASSERT_TRUE(rest.cancel_order(cmd, btc_usdt(), k_now).has_value());
    const auto cancel = ws.request(10, k_ws_order_cancel, rest.last_sign_message());
    ASSERT_TRUE(cancel.has_value());
    EXPECT_NE(cancel->find(R"("method":"order.cancel")"), std::string_view::npos);
    EXPECT_NE(cancel->find(R"("orderId":336829446,)"), std::string_view::npos);

    ASSERT_TRUE(rest.amend_order(cmd, btc_usdt(), k_now).has_value());
    const auto modify = ws.request(11, k_ws_order_modify, rest.last_sign_message());
    ASSERT_TRUE(modify.has_value());
    EXPECT_NE(modify->find(R"("method":"order.modify")"), std::string_view::npos);
    EXPECT_NE(modify->find(R"("side":"BUY")"), std::string_view::npos);
}

TEST(BinanceWsApi, PercentEncodedValuesTravelDecoded) {
    net::Signer signer;
    give_credentials(signer);
    WsApiBuilder ws{signer};
    const auto frame = ws.request(1, k_ws_order_cancel, "symbol=%E5%B8%81USDT&origClientOrderId=X&timestamp=1");
    ASSERT_TRUE(frame.has_value());
    EXPECT_NE(frame->find("\"symbol\":\"\xE5\xB8\x81USDT\""), std::string_view::npos);
}

TEST(BinanceWsApi, WithoutCredentialsNothingIsBuiltAndTheChannelCannotOpen) {
    net::Signer signer;
    WsApiBuilder ws{signer};
    EXPECT_EQ(ws.request(1, k_ws_order_place, "symbol=BTCUSDT").error(), BuildError::NoCredentials);
    WsApiCodec codec{signer};
    EXPECT_FALSE(codec.login(k_now).has_value());
}

TEST(BinanceWsApi, DocumentedSuccessRepliesCarryNoError) {
    for (const auto* name : {"ws_api_order_place.json", "ws_api_order_cancel.json", "ws_api_order_modify.json"}) {
        const auto reply = parse_ws_api_reply(with_id(fixture(name), "42"));
        ASSERT_TRUE(reply.has_value()) << name;
        EXPECT_EQ(reply->id, 42U) << name;
        EXPECT_EQ(reply->status, 200U) << name;
        EXPECT_EQ(reply->error.code, k_ok) << name;
    }
}

TEST(BinanceWsApi, DocumentedRefusalsCarryTheVenueCode) {
    const auto rejected = parse_ws_api_reply(fixture("ws_api_error.json"));
    ASSERT_TRUE(rejected.has_value());
    EXPECT_EQ(rejected->status, 400U);
    EXPECT_EQ(rejected->error.code, k_mandatory_param_empty);
    EXPECT_EQ(classify(rejected->error.code), exec::ErrorClass::Terminal);

    const auto auth = parse_ws_api_reply(fixture("ws_api_auth_failure.json"));
    ASSERT_TRUE(auth.has_value());
    EXPECT_EQ(auth->error.code, k_invalid_signature);
    EXPECT_EQ(classify(auth->error.code), exec::ErrorClass::AuthFailure);
}

TEST(BinanceWsApi, OnlyOurNumericIdsCountAsReplies) {
    net::Signer signer;
    give_credentials(signer);
    WsApiCodec codec{signer};
    EXPECT_EQ(codec.classify(fixture("ws_api_order_place.json")).kind, net::WsRpcFrame::Kind::Other);
    const auto ours = codec.classify(with_id(fixture("ws_api_error.json"), "7"));
    EXPECT_EQ(ours.kind, net::WsRpcFrame::Kind::Reply);
    EXPECT_EQ(ours.id, 7U);
    EXPECT_EQ(codec.classify("not json").kind, net::WsRpcFrame::Kind::Other);
}

class BinanceWsRoute : public ::testing::Test {
protected:
    void SetUp() override {
        give_credentials(signer);
        ASSERT_TRUE(rest.create_order(limit_buy(), btc_usdt(), k_now).has_value());
    }

    // the adapter's routing: rest runs only when the socket did not take the command
    void route() {
        const auto id = rpc.next_id();
        const auto frame = ws.request(id, k_ws_order_place, rest.last_sign_message());
        ASSERT_TRUE(frame.has_value());
        if (!rpc.request(id, *frame, [](const net::WsRpcReply&) {})) {
            ++rest_sends;
        }
    }

    TestClock clock{k_now, MonoNanos{1}};
    net::Signer signer;
    RequestBuilder rest{signer};
    WsApiBuilder ws{signer};
    WsApiCodec codec{signer};
    std::vector<std::string> socket;
    bool socket_up{true};
    int rest_sends{0};
    net::WsRpc rpc{clock, net::WsRpcConfig{}, codec, [this](std::string_view frame) {
                       if (!socket_up) {
                           return false;
                       }
                       socket.emplace_back(frame);
                       return true;
                   }};
};

TEST_F(BinanceWsRoute, ASocketThatIsNotOpenSendsTheOrderOverRestOnly) {
    route();
    EXPECT_EQ(rest_sends, 1);
    EXPECT_TRUE(socket.empty());
}

TEST_F(BinanceWsRoute, AnOpenSocketTakesTheOrderAndRestNeverSeesIt) {
    rpc.start();
    rpc.on_open();
    ASSERT_TRUE(rpc.ready());
    route();
    EXPECT_EQ(rest_sends, 0);
    EXPECT_EQ(socket.size(), 1U);
}

TEST_F(BinanceWsRoute, ARefusedWriteGoesToRestAndLeavesNothingToResend) {
    rpc.start();
    rpc.on_open();
    socket_up = false;
    route();
    EXPECT_EQ(rest_sends, 1);
    EXPECT_EQ(rpc.in_flight(), 0U);
    socket_up = true;
    rpc.on_close(net::WsCloseReason::Transport);
    rpc.on_open();
    EXPECT_TRUE(socket.empty());
}

} // namespace
} // namespace raikiri::venues::binance
