#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/kucoin/catalogue.hpp>
#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/venues/kucoin/map.hpp>
#include <raikiri/venues/kucoin/rest.hpp>
#include <raikiri/venues/kucoin/trade.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::kucoin {
namespace {

// kucoin signs milliseconds: 1730211033335
constexpr UnixNanos k_now{1'730'211'033'335'000'000ULL};
constexpr std::uint64_t k_ms = 1'000'000ULL;

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    auto text = buf.str();
    while (!text.empty() && text.back() == '\n') {
        text.pop_back();
    }
    return text;
}

// a doc example under one of our numeric ids
std::string with_id(std::string frame, std::string_view id) {
    const auto start = frame.find(R"("id":")") + 6;
    const auto end = frame.find('"', start);
    frame.replace(start, end - start, id);
    return frame;
}

exec::InstrumentTable futures_table() {
    const auto body = fixture("rest_contracts_active.json");
    static ContractBuffer storage;
    const auto wire = parse_contracts(body, storage);
    EXPECT_TRUE(wire.has_value());
    exec::InstrumentTable table;
    for (const auto& entry : *wire) {
        if (auto instrument = map_instrument(entry, MarketType::Perp); instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value());
        }
    }
    return table;
}

exec::InstrumentTable spot_table() {
    const auto body = fixture("rest_spot_symbols.json");
    static SpotSymbolBuffer storage;
    const auto wire = parse_spot_symbols(body, storage);
    EXPECT_TRUE(wire.has_value());
    exec::InstrumentTable table;
    for (const auto& entry : *wire) {
        if (auto instrument = map_spot_symbol(entry); instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value());
        }
    }
    return table;
}

void load_credentials(Credentials& credentials) {
    credentials.signer().set_api_key("kucoin-test-key");
    ASSERT_TRUE(credentials.signer().secret().assign("kucoin-test-secret").has_value());
    ASSERT_TRUE(credentials.seal_passphrase("kucoin-test-passphrase"));
}

OrderCmd limit_buy(const exec::InstrumentTable& table, std::string_view symbol) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.client_order_id = ClientOrderId{77};
    cmd.instrument = base_of(*table.find(symbol)).id;
    cmd.side = Side::Buy;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.price = PriceTicks::from_raw(641'256);
    cmd.qty = QtyLots::from_raw(3);
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("01J8ZQK9V7M3XPB2R6TDNAWC5E");
    return cmd;
}

struct Sink final : exec::GatewaySink {
    void on_bbo(const Bbo&) noexcept override {}

    void on_trade(const TradeTick&) noexcept override {}

    void on_funding(const FundingUpdate&) noexcept override {}

    void on_order_event(const OrderEvent& event) noexcept override { events.push_back(event); }

    void on_order_error(ClientOrderId, const exec::VenueError& error) noexcept override { errors.push_back(error); }

    void on_connectivity(const exec::ConnEvent&) noexcept override {}

    std::vector<OrderEvent> events;
    std::vector<exec::VenueError> errors;
};

TEST(KucoinTrade, AFuturesOrderCarriesTheRestBodyAsItsArgs) {
    const auto table = futures_table();
    Credentials credentials;
    load_credentials(credentials);
    OrderRequestBuilder rest{credentials};
    const auto request = rest.create_order(limit_buy(table, "XBTUSDTM"), table, k_now);
    ASSERT_TRUE(request.has_value());

    TradeFrameBuilder ws;
    const auto frame = ws.place(17, MarketType::Perp, request->body);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(*frame, R"({"id":"17","op":"futures.order","args":)" + std::string{request->body} + "}");
    EXPECT_NE(frame->find(R"("clientOid":"01J8ZQK9V7M3XPB2R6TDNAWC5E")"), std::string_view::npos);
    EXPECT_EQ(ws.place(18, MarketType::Option, request->body).error(), BuildError::UnsupportedMarket);
}

TEST(KucoinTrade, ASpotOrderUsesTheSpotOp) {
    const auto table = spot_table();
    TradeFrameBuilder ws;
    const auto frame = ws.place(19, MarketType::Spot, R"({"clientOid":"x"})");
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(*frame, R"({"id":"19","op":"spot.order","args":{"clientOid":"x"}})");
    const auto cancel = ws.cancel(20, limit_buy(table, "BTC-USDT"), table);
    ASSERT_TRUE(cancel.has_value());
    EXPECT_EQ(
        *cancel,
        R"({"id":"20","op":"spot.cancel","args":{"clientOid":"01J8ZQK9V7M3XPB2R6TDNAWC5E","symbol":"BTC-USDT"}})");
}

TEST(KucoinTrade, AFuturesCancelNamesTheClientIdUnlessTheVenueIdIsKnown) {
    const auto table = futures_table();
    TradeFrameBuilder ws;
    auto cmd = limit_buy(table, "XBTUSDTM");
    const auto by_client = ws.cancel(21, cmd, table);
    ASSERT_TRUE(by_client.has_value());
    EXPECT_EQ(
        *by_client,
        R"({"id":"21","op":"futures.cancel","args":{"clientOid":"01J8ZQK9V7M3XPB2R6TDNAWC5E","symbol":"XBTUSDTM"}})");
    cmd.target_exchange_order_id = *ExchangeOrderId::from("403058726287794177");
    const auto by_order = ws.cancel(22, cmd, table);
    ASSERT_TRUE(by_order.has_value());
    EXPECT_NE(by_order->find(R"("args":{"orderId":"403058726287794177","symbol":"XBTUSDTM"})"), std::string_view::npos);
    cmd.target_exchange_order_id = {};
    cmd.exchange_client_order_id = {};
    EXPECT_EQ(ws.cancel(23, cmd, table).error(), BuildError::BadClientOrderId);
}

TEST(KucoinTrade, TheTargetIsSignedPerConnectionAndTheLoginWaitsForTheSession) {
    Credentials none;
    TradeCodec without{none};
    EXPECT_TRUE(without.target(k_now).empty());
    EXPECT_FALSE(without.login(k_now).has_value());

    Credentials credentials;
    load_credentials(credentials);
    TradeCodec codec{credentials};
    // hmac-sha256 base64 of key + timestamp and of the passphrase, escaped for a query
    EXPECT_EQ(codec.target(k_now),
              "/v1/private?apikey=kucoin-test-key&timestamp=1730211033335"
              "&sign=5m0CKKfE3vv4noOdrpw%2BHufqr6%2FbQXSq47FSlKDmx08%3D"
              "&passphrase=WX%2FMCPps2NpCAWajYjOod%2FNfgMDWvFoHPQ15hoWy%2BfE%3D");
    EXPECT_NE(codec.target(UnixNanos{k_now.ns + 1'000 * k_ms}), codec.target(k_now));
    const auto login = codec.login(k_now);
    ASSERT_TRUE(login.has_value());
    EXPECT_TRUE(login->empty());
    EXPECT_TRUE(codec.challenged());
}

TEST(KucoinTrade, TheDocumentedSessionIsSignedBackAndTheWelcomeSettlesTheLogin) {
    Credentials credentials;
    load_credentials(credentials);
    TradeCodec codec{credentials};
    const auto session = fixture("ws_trade_session.json");
    EXPECT_EQ(codec.classify(session).kind, net::WsRpcFrame::Kind::Challenge);
    EXPECT_EQ(codec.answer(session), "HPMVEJqqEkDAad9xueD14pI43XsBwUsvzbqJr27LZFs=");

    const auto welcome = codec.classify(fixture("ws_trade_welcome.json"));
    EXPECT_EQ(welcome.kind, net::WsRpcFrame::Kind::Login);
    EXPECT_TRUE(welcome.ok);
    const auto error = fixture("ws_trade_connection_error.json");
    const auto refused = codec.classify(error);
    EXPECT_EQ(refused.kind, net::WsRpcFrame::Kind::Login);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.detail, "KC-API-KEY not exists.");
    EXPECT_EQ(codec.classify(fixture("ws_trade_pong.json")).kind, net::WsRpcFrame::Kind::Pong);
}

TEST(KucoinTrade, DocumentedRepliesAreMatchedByIdAndKeepTheVenueCode) {
    Credentials credentials;
    TradeCodec codec{credentials};
    EXPECT_EQ(codec.classify(fixture("ws_trade_futures_order.json")).kind, net::WsRpcFrame::Kind::Other);
    for (const auto* name : {"ws_trade_futures_order.json",
                             "ws_trade_spot_order.json",
                             "ws_trade_futures_cancel.json",
                             "ws_trade_spot_cancel.json"}) {
        const auto accepted = codec.classify(with_id(fixture(name), "17"));
        EXPECT_EQ(accepted.kind, net::WsRpcFrame::Kind::Reply) << name;
        EXPECT_EQ(accepted.id, 17U) << name;
        EXPECT_TRUE(accepted.ok) << name;
    }
    const auto error = with_id(fixture("ws_trade_order_error.json"), "18");
    EXPECT_FALSE(codec.classify(error).ok);
    const auto parsed = parse_trade_reply(error);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->id, 18U);
    EXPECT_EQ(parsed->op, k_op_futures_order);
    EXPECT_EQ(parsed->code, 100003);
    EXPECT_EQ(parsed->msg, "Contract parameter invalid.");
    const auto limited = parse_trade_reply(with_id(fixture("ws_trade_rate_limit.json"), "19"));
    ASSERT_TRUE(limited.has_value());
    EXPECT_EQ(classify(limited->code), exec::ErrorClass::RateLimited);
    EXPECT_EQ(codec.classify("not json").kind, net::WsRpcFrame::Kind::Other);
}

class KucoinTradeRoute : public ::testing::Test {
protected:
    void SetUp() override { load_credentials(credentials); }

    // the adapter's routing: rest runs only when the socket did not take the command
    void route(const OrderCmd& cmd) {
        const auto id = rpc.next_id();
        const auto frame = ws.cancel(id, cmd, table);
        ASSERT_TRUE(frame.has_value());
        if (!rpc.request(id, *frame, [this, cmd](const net::WsRpcReply& reply) {
                if (reply.outcome == net::WsRpcOutcome::Unknown) {
                    exec::report_unknown_outcome(
                        sink, exec::in_flight_of(cmd, clock.mono()), clock.wall(), reply.detail);
                }
            })) {
            ++rest_sends;
        }
    }

    void log_in() {
        rpc.start();
        rpc.on_open();
        ASSERT_TRUE(socket.empty());
        rpc.on_message(fixture("ws_trade_session.json"), {});
        ASSERT_EQ(socket.size(), 1U);
        rpc.on_message(fixture("ws_trade_welcome.json"), {});
        ASSERT_TRUE(rpc.ready());
        socket.clear();
    }

    exec::InstrumentTable table{futures_table()};
    Credentials credentials;
    TradeCodec codec{credentials};
    TradeFrameBuilder ws;
    TestClock clock{k_now, MonoNanos{1}};
    Sink sink;
    std::vector<std::string> socket;
    bool socket_up{true};
    int rest_sends{0};
    net::WsRpc rpc{clock, net::WsRpcConfig{.request_timeout_ns = 500 * k_ms}, codec, [this](std::string_view frame) {
                       if (!socket_up) {
                           return false;
                       }
                       socket.emplace_back(frame);
                       return true;
                   }};
};

TEST_F(KucoinTradeRoute, NothingReachesTheSocketBeforeTheWelcomeAndRestTakesItInstead) {
    rpc.start();
    rpc.on_open();
    EXPECT_EQ(rpc.state(), net::WsRpcState::LoggingIn);
    route(limit_buy(table, "XBTUSDTM"));
    rpc.on_message(fixture("ws_trade_session.json"), {});
    route(limit_buy(table, "XBTUSDTM"));
    EXPECT_EQ(rest_sends, 2);
    ASSERT_EQ(socket.size(), 1U);
    EXPECT_EQ(socket[0], "HPMVEJqqEkDAad9xueD14pI43XsBwUsvzbqJr27LZFs=");
}

TEST_F(KucoinTradeRoute, ARefusedConnectionWaitsBeforeRetrying) {
    rpc.start();
    rpc.on_open();
    rpc.on_message(fixture("ws_trade_connection_error.json"), {});
    EXPECT_EQ(rpc.state(), net::WsRpcState::AuthFailed);
    route(limit_buy(table, "XBTUSDTM"));
    EXPECT_EQ(rest_sends, 1);
}

TEST_F(KucoinTradeRoute, AReadySocketTakesTheCancelAndRestNeverSeesIt) {
    log_in();
    route(limit_buy(table, "XBTUSDTM"));
    EXPECT_EQ(rest_sends, 0);
    EXPECT_EQ(socket.size(), 1U);
    rpc.on_message(with_id(fixture("ws_trade_futures_cancel.json"), "1"), {});
    EXPECT_EQ(rpc.in_flight(), 0U);
    EXPECT_TRUE(sink.events.empty());
}

TEST_F(KucoinTradeRoute, ARefusedWriteGoesToRestOnceAndIsNeverResent) {
    log_in();
    socket_up = false;
    route(limit_buy(table, "XBTUSDTM"));
    EXPECT_EQ(rest_sends, 1);
    EXPECT_EQ(rpc.in_flight(), 0U);
    socket_up = true;
    rpc.on_close(net::WsCloseReason::Transport);
    rpc.on_open();
    EXPECT_TRUE(socket.empty());
}

TEST_F(KucoinTradeRoute, ATimeoutEndsUnknownWithAReconGap) {
    log_in();
    route(limit_buy(table, "XBTUSDTM"));
    clock.advance(499 * k_ms);
    rpc.poll();
    EXPECT_TRUE(sink.events.empty());
    clock.advance(2 * k_ms);
    rpc.poll();
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::ReconGap);
    EXPECT_EQ(sink.events[0].client_order_id, ClientOrderId{77});
    EXPECT_TRUE(sink.events[0].leaves_qty.is_undef());
    ASSERT_EQ(sink.errors.size(), 1U);
    EXPECT_EQ(sink.errors[0].cls, exec::ErrorClass::Connectivity);
    rpc.on_message(with_id(fixture("ws_trade_futures_cancel.json"), "1"), {});
    EXPECT_EQ(sink.events.size(), 1U);
}

TEST_F(KucoinTradeRoute, ADropMidFlightEndsUnknownAndTheNextSocketStartsWithTheSession) {
    log_in();
    route(limit_buy(table, "XBTUSDTM"));
    ASSERT_EQ(rpc.in_flight(), 1U);
    rpc.on_close(net::WsCloseReason::Transport);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::ReconGap);
    route(limit_buy(table, "XBTUSDTM"));
    EXPECT_EQ(rest_sends, 1);
    socket.clear();
    rpc.on_open();
    EXPECT_TRUE(socket.empty());
    EXPECT_EQ(rpc.state(), net::WsRpcState::LoggingIn);
}

} // namespace
} // namespace raikiri::venues::kucoin
