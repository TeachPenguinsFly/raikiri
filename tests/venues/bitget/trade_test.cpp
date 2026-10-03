#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/bitget/catalogue.hpp>
#include <raikiri/venues/bitget/errors.hpp>
#include <raikiri/venues/bitget/map.hpp>
#include <raikiri/venues/bitget/rest.hpp>
#include <raikiri/venues/bitget/trade.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::bitget {
namespace {

constexpr UnixNanos k_now{1'700'000'000'123'000'000ULL};
constexpr std::uint64_t k_ms = 1'000'000ULL;

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
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

exec::InstrumentTable make_table() {
    exec::InstrumentTable table;
    auto storage = std::make_unique<ContractBuffer>();
    const auto body = fixture("rest_contracts.json");
    const auto rows = parse_contracts(body, *storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& row : *rows) {
        if (const auto instrument = map_instrument(row, MarketType::Perp); instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value());
        }
    }
    return table;
}

OrderCmd limit_buy(const exec::InstrumentTable& table) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.client_order_id = ClientOrderId{77};
    cmd.instrument = base_of(*table.find(std::string_view{"BTCUSDT"})).id;
    cmd.side = Side::Buy;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.flags = OrderFlags::PostOnly;
    cmd.price = PriceTicks::from_raw(600'000);
    cmd.qty = QtyLots::from_raw(2);
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("01ARZ3NDEKTSV4RRFFQ69G5FAV");
    return cmd;
}

struct Credentials {
    Credentials() {
        signer.set_api_key("bitget-test-key");
        EXPECT_TRUE(signer.secret().assign("bitget-test-secret").has_value());
        EXPECT_TRUE(passphrase.assign("bitget-test-passphrase").has_value());
    }

    net::Signer signer;
    config::Secret passphrase;
    std::int64_t offset_ns{0};
};

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

TEST(BitgetTrade, APlaceFrameCarriesTheRestFieldsWithoutSymbolOrProduct) {
    const auto table = make_table();
    Credentials keys;
    RequestBuilder rest;
    rest.set_credentials(keys.signer, keys.passphrase.view());
    const auto request = rest.create_order(limit_buy(table), table, product_type(false), k_now);
    ASSERT_TRUE(request.has_value());
    const std::string_view body = request->body;
    const auto prefix = std::string_view{R"({"symbol":"BTCUSDT","productType":"USDT-FUTURES",)"};
    ASSERT_TRUE(body.starts_with(prefix)) << body;

    TradeFrameBuilder ws;
    const auto frame = ws.place(17, limit_buy(table), table, product_type(false), inst_type(MarketType::Perp, false));
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(*frame,
              R"({"op":"trade","args":[{"id":"17","instType":"USDT-FUTURES","instId":"BTCUSDT",)"
              R"("channel":"place-order","params":{)" +
                  std::string{body.substr(prefix.size())} + "}]}");
    EXPECT_NE(frame->find(R"("force":"post_only")"), std::string_view::npos);
    EXPECT_NE(frame->find(R"("clientOid":"01ARZ3NDEKTSV4RRFFQ69G5FAV")"), std::string_view::npos);
}

TEST(BitgetTrade, ACancelNamesTheClientIdUnlessTheVenueIdIsKnown) {
    const auto table = make_table();
    TradeFrameBuilder ws;
    auto cmd = limit_buy(table);
    const auto by_client = ws.cancel(18, cmd, table, inst_type(MarketType::Perp, false));
    ASSERT_TRUE(by_client.has_value());
    EXPECT_EQ(*by_client,
              R"({"op":"trade","args":[{"id":"18","instType":"USDT-FUTURES","instId":"BTCUSDT",)"
              R"("channel":"cancel-order","params":{"clientOid":"01ARZ3NDEKTSV4RRFFQ69G5FAV"}}]})");
    cmd.target_exchange_order_id = *ExchangeOrderId::from("1234567890");
    const auto by_order = ws.cancel(19, cmd, table, inst_type(MarketType::Perp, false));
    ASSERT_TRUE(by_order.has_value());
    EXPECT_NE(by_order->find(R"("params":{"orderId":"1234567890"})"), std::string_view::npos);
}

TEST(BitgetTrade, AnOrderTheRestBuilderRefusesIsRefusedTheSame) {
    const auto table = make_table();
    auto cmd = limit_buy(table);
    cmd.order_type = OrderType::Market;
    RequestBuilder rest;
    TradeFrameBuilder ws;
    EXPECT_EQ(rest.create_order(cmd, table, product_type(false), k_now).error(), BuildError::UnsupportedOrder);
    EXPECT_EQ(ws.place(1, cmd, table, product_type(false), inst_type(MarketType::Perp, false)).error(),
              BuildError::UnsupportedOrder);
    // demo spot has no instType, so the socket cannot carry it
    EXPECT_EQ(ws.cancel(2, limit_buy(table), table, {}).error(), BuildError::UnsupportedMarket);
}

TEST(BitgetTrade, LoginIsTheSignedLoginOpAtVenueTimeAndNeedsAKey) {
    net::Signer no_key;
    config::Secret no_passphrase;
    std::int64_t zero = 0;
    TradeCodec without{no_key, no_passphrase, zero};
    EXPECT_FALSE(without.login(k_now).has_value());

    Credentials keys;
    keys.offset_ns = 5'000'000'000LL;
    TradeCodec codec{keys.signer, keys.passphrase, keys.offset_ns};
    const auto frame = codec.login(k_now);
    ASSERT_TRUE(frame.has_value());
    EXPECT_NE(frame->find(R"("op":"login")"), std::string::npos);
    EXPECT_NE(frame->find(R"("timestamp":"1700000005")"), std::string::npos) << *frame;
}

TEST(BitgetTrade, DocumentedLoginRepliesSettleTheLogin) {
    Credentials keys;
    TradeCodec codec{keys.signer, keys.passphrase, keys.offset_ns};
    const auto ok = codec.classify(fixture("ws_login_ack.json"));
    EXPECT_EQ(ok.kind, net::WsRpcFrame::Kind::Login);
    EXPECT_TRUE(ok.ok);
    const auto failure = fixture("ws_login_failure.json");
    const auto refused = codec.classify(failure);
    EXPECT_EQ(refused.kind, net::WsRpcFrame::Kind::Login);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(codec.classify("pong").kind, net::WsRpcFrame::Kind::Pong);
    EXPECT_EQ(codec.ping(), "ping");
}

TEST(BitgetTrade, DocumentedRepliesAreMatchedByIdAndKeepTheVenueCode) {
    Credentials keys;
    TradeCodec codec{keys.signer, keys.passphrase, keys.offset_ns};
    EXPECT_EQ(codec.classify(fixture("ws_trade_place_order.json")).kind, net::WsRpcFrame::Kind::Other);
    for (const auto* name :
         {"ws_trade_place_order.json", "ws_trade_cancel_order.json", "ws_trade_spot_place_order.json"}) {
        const auto frame = with_id(fixture(name), "17");
        const auto accepted = codec.classify(frame);
        EXPECT_EQ(accepted.kind, net::WsRpcFrame::Kind::Reply) << name;
        EXPECT_EQ(accepted.id, 17U) << name;
        EXPECT_TRUE(accepted.ok) << name;
        const auto parsed = parse_trade_reply(frame);
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->code, k_ws_ok);
    }

    const auto error = with_id(fixture("ws_trade_error.json"), "18");
    const auto refused = codec.classify(error);
    EXPECT_EQ(refused.kind, net::WsRpcFrame::Kind::Reply);
    EXPECT_EQ(refused.id, 18U);
    EXPECT_FALSE(refused.ok);
    const auto parsed = parse_trade_reply(error);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->event, "error");
    EXPECT_EQ(parsed->code, k_parameter_verification);
    EXPECT_EQ(parsed->msg, "Parameter verification failed");
    EXPECT_EQ(codec.classify("not json").kind, net::WsRpcFrame::Kind::Other);
}

class BitgetTradeRoute : public ::testing::Test {
protected:
    // the adapter's routing: rest runs only when the socket did not take the command
    void route(const OrderCmd& cmd) {
        const auto id = rpc.next_id();
        const auto frame = ws.place(id, cmd, table, product_type(false), inst_type(MarketType::Perp, false));
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
        ASSERT_EQ(socket.size(), 1U);
        rpc.on_message(fixture("ws_login_ack.json"), {});
        ASSERT_TRUE(rpc.ready());
        socket.clear();
    }

    exec::InstrumentTable table{make_table()};
    Credentials keys;
    TradeCodec codec{keys.signer, keys.passphrase, keys.offset_ns};
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

TEST_F(BitgetTradeRoute, NothingReachesTheSocketBeforeTheLoginReplyAndRestTakesItInstead) {
    rpc.start();
    rpc.on_open();
    ASSERT_EQ(socket.size(), 1U);
    route(limit_buy(table));
    EXPECT_EQ(rest_sends, 1);
    EXPECT_EQ(socket.size(), 1U);
}

TEST_F(BitgetTradeRoute, AReadySocketTakesTheOrderAndRestNeverSeesIt) {
    log_in();
    route(limit_buy(table));
    EXPECT_EQ(rest_sends, 0);
    EXPECT_EQ(socket.size(), 1U);
    rpc.on_message(with_id(fixture("ws_trade_place_order.json"), "1"), {});
    EXPECT_EQ(rpc.in_flight(), 0U);
    EXPECT_TRUE(sink.events.empty());
}

TEST_F(BitgetTradeRoute, ARefusedWriteGoesToRestOnceAndIsNeverResent) {
    log_in();
    socket_up = false;
    route(limit_buy(table));
    EXPECT_EQ(rest_sends, 1);
    EXPECT_EQ(rpc.in_flight(), 0U);
    socket_up = true;
    rpc.on_close(net::WsCloseReason::Transport);
    rpc.on_open();
    EXPECT_EQ(socket.size(), 1U);
    EXPECT_NE(socket[0].find(R"("op":"login")"), std::string::npos);
}

TEST_F(BitgetTradeRoute, ATimeoutEndsUnknownWithAReconGap) {
    log_in();
    route(limit_buy(table));
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
    rpc.on_message(with_id(fixture("ws_trade_place_order.json"), "1"), {});
    EXPECT_EQ(sink.events.size(), 1U);
}

TEST_F(BitgetTradeRoute, ADropMidFlightEndsUnknownAndTheNextSocketCarriesOnlyTheLogin) {
    log_in();
    route(limit_buy(table));
    ASSERT_EQ(rpc.in_flight(), 1U);
    rpc.on_close(net::WsCloseReason::Transport);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::ReconGap);
    EXPECT_EQ(rest_sends, 0);
    route(limit_buy(table));
    EXPECT_EQ(rest_sends, 1);
    socket.clear();
    rpc.on_open();
    ASSERT_EQ(socket.size(), 1U);
    EXPECT_NE(socket[0].find(R"("op":"login")"), std::string::npos);
}

} // namespace
} // namespace raikiri::venues::bitget
