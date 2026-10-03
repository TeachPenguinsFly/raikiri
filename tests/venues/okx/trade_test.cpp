#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/okx/catalogue.hpp>
#include <raikiri/venues/okx/envelope.hpp>
#include <raikiri/venues/okx/map.hpp>
#include <raikiri/venues/okx/orders.hpp>
#include <raikiri/venues/okx/rest.hpp>
#include <raikiri/venues/okx/trade.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::okx {
namespace {

constexpr UnixNanos k_now{1'700'000'000'123'000'000ULL};
constexpr std::uint64_t k_ms = 1'000'000ULL;
constexpr std::uint64_t k_btc_code = 10459;
constexpr std::int64_t k_no_offset = 0;

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
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

exec::InstrumentTable table_with_btc() {
    exec::InstrumentTable table;
    WireInstrument wire{};
    wire.inst_id = "BTC-USDT-SWAP";
    wire.inst_type = "SWAP";
    wire.state = "live";
    wire.tick_size = "0.1";
    wire.lot_size = "0.01";
    wire.min_size = "0.01";
    wire.ct_val = "0.01";
    wire.ct_mult = "1";
    wire.ct_type = "linear";
    const auto instrument = map_instrument(wire, MarketType::Perp);
    EXPECT_TRUE(instrument.has_value());
    EXPECT_TRUE(table.add(*instrument).has_value());
    return table;
}

OrderCmd limit_buy(const exec::InstrumentTable& table) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.instrument = base_of(*table.find(std::string_view{"BTC-USDT-SWAP"})).id;
    cmd.side = Side::Buy;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.price = PriceTicks::from_raw(600'000);
    cmd.qty = QtyLots::from_raw(3);
    cmd.client_order_id = ClientOrderId{77};
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("01ARZ3NDEKTSV4RRFFQ69G5FAV");
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

TEST(OkxTrade, TheCatalogueKeepsTheCodeTheSocketNeedsAndNullReadsAsNone) {
    const auto body = fixture("rest_instruments.json");
    InstrumentBuffer storage;
    const auto rows = parse_instruments(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);
    EXPECT_EQ((*rows)[0].inst_id_code, k_btc_code);
    EXPECT_EQ((*rows)[2].inst_id, "JP225-USDT-SWAP");
    EXPECT_EQ((*rows)[2].inst_id_code, 0U);
}

TEST(OkxTrade, AnOrderArgIsTheRestBodyWithTheInstrumentCodeFirst) {
    const auto table = table_with_btc();
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder rest{signer};
    rest.set_passphrase("pass");
    const auto request = rest.create_order(limit_buy(table), table, k_now);
    ASSERT_TRUE(request.has_value());

    TradeFrameBuilder ws;
    const auto frame = ws.request(17, k_op_order, request->body, k_btc_code);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(*frame,
              R"({"id":"17","op":"order","args":[{"instIdCode":10459,)" + std::string{request->body.substr(1)} + "]}");
    EXPECT_NE(frame->find(R"("clOrdId":"01ARZ3NDEKTSV4RRFFQ69G5FAV")"), std::string_view::npos);
}

TEST(OkxTrade, CancelAndAmendUseTheirOwnOpsAndAMissingCodeBuildsNothing) {
    const auto table = table_with_btc();
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder rest{signer};
    rest.set_passphrase("pass");
    TradeFrameBuilder ws;

    const auto cancel = rest.cancel_order(limit_buy(table), table, k_now);
    ASSERT_TRUE(cancel.has_value());
    const auto cancel_frame = ws.request(18, k_op_cancel, cancel->body, k_btc_code);
    ASSERT_TRUE(cancel_frame.has_value());
    EXPECT_TRUE(cancel_frame->starts_with(R"({"id":"18","op":"cancel-order","args":[{"instIdCode":10459,)"));

    const auto amend = rest.amend_order(limit_buy(table), table, k_now);
    ASSERT_TRUE(amend.has_value());
    const auto amend_frame = ws.request(19, k_op_amend, amend->body, k_btc_code);
    ASSERT_TRUE(amend_frame.has_value());
    EXPECT_NE(amend_frame->find(R"("op":"amend-order")"), std::string_view::npos);

    EXPECT_FALSE(ws.request(20, k_op_order, cancel->body, 0).has_value());
}

TEST(OkxTrade, LoginIsTheSignedLoginOpAtVenueTime) {
    net::Signer signer;
    const std::int64_t offset = 5'000'000'000LL;
    TradeCodec without{signer, "pass", offset};
    EXPECT_FALSE(without.login(k_now).has_value());

    load_credentials(signer);
    TradeCodec codec{signer, "pass", offset};
    const auto frame = codec.login(k_now);
    ASSERT_TRUE(frame.has_value());
    EXPECT_TRUE(frame->starts_with(R"({"op":"login","args":[{"apiKey":"TESTKEY123",)"));
    EXPECT_NE(frame->find(R"("timestamp":"1700000005")"), std::string::npos);
}

TEST(OkxTrade, DocumentedLoginRepliesSettleTheLogin) {
    net::Signer signer;
    TradeCodec codec{signer, "pass", k_no_offset};
    const auto ok = codec.classify(fixture("ws_login_ok.json"));
    EXPECT_EQ(ok.kind, net::WsRpcFrame::Kind::Login);
    EXPECT_TRUE(ok.ok);
    const auto failure = fixture("ws_login_failed.json");
    const auto refused = codec.classify(failure);
    EXPECT_EQ(refused.kind, net::WsRpcFrame::Kind::Login);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.detail, "Login failed.");
}

TEST(OkxTrade, DocumentedRepliesAreMatchedByIdAndReadAsRestWouldReadThem) {
    net::Signer signer;
    TradeCodec codec{signer, "pass", k_no_offset};
    const auto placed = fixture("ws_trade_order.json");
    const auto accepted = codec.classify(placed);
    EXPECT_EQ(accepted.kind, net::WsRpcFrame::Kind::Reply);
    EXPECT_EQ(accepted.id, 1512U);
    EXPECT_TRUE(accepted.ok);
    EXPECT_EQ(codec.classify(fixture("ws_trade_cancel.json")).id, 1514U);

    const auto refused = fixture("ws_trade_order_failure.json");
    EXPECT_FALSE(codec.classify(refused).ok);
    const auto envelope = parse_rest_envelope(refused);
    ASSERT_TRUE(envelope.has_value());
    EXPECT_EQ(envelope->code, 1);

    const auto amend = fixture("ws_trade_amend_failure.json");
    AckBuffer storage;
    const auto acks = parse_order_acks(amend, storage);
    ASSERT_TRUE(acks.has_value());
    ASSERT_EQ(acks->size(), 1U);
    EXPECT_EQ((*acks)[0].s_code, 51008);
    EXPECT_EQ((*acks)[0].s_msg, "Order failed. Insufficient USDT balance in account");

    const auto malformed = fixture("ws_trade_format_error.json");
    const auto format = codec.classify(malformed);
    EXPECT_EQ(format.kind, net::WsRpcFrame::Kind::Reply);
    EXPECT_FALSE(format.ok);
    EXPECT_EQ(format.detail, "Invalid args");
}

TEST(OkxTrade, TheBarePongIsTimed) {
    net::Signer signer;
    TradeCodec codec{signer, "pass", k_no_offset};
    EXPECT_EQ(codec.classify("pong").kind, net::WsRpcFrame::Kind::Pong);
    EXPECT_EQ(codec.ping(), "ping");
    EXPECT_EQ(codec.classify("not json").kind, net::WsRpcFrame::Kind::Other);
}

class OkxTradeRoute : public ::testing::Test {
protected:
    void SetUp() override {
        load_credentials(signer);
        rest.set_passphrase("pass");
    }

    static net::WsRpcConfig config() {
        net::WsRpcConfig cfg{};
        cfg.request_timeout_ns = 500 * k_ms;
        return cfg;
    }

    void log_in() {
        rpc.start();
        rpc.on_open();
        ASSERT_EQ(socket.size(), 1U);
        rpc.on_message(fixture("ws_login_ok.json"), {});
        ASSERT_TRUE(rpc.ready());
        socket.clear();
    }

    // the adapter's routing: rest runs only when the socket did not take the command
    void route(std::uint64_t code = k_btc_code) {
        const auto request = rest.create_order(cmd, table, k_now);
        ASSERT_TRUE(request.has_value());
        const auto id = rpc.next_id();
        const auto frame = ws.request(id, k_op_order, request->body, code);
        const bool taken = rpc.ready() && frame.has_value() &&
                           rpc.request(id, *frame, [this](const net::WsRpcReply& reply) {
                               if (reply.outcome == net::WsRpcOutcome::Unknown) {
                                   exec::report_unknown_outcome(
                                       sink, exec::in_flight_of(cmd, clock.mono()), clock.wall(), reply.detail);
                               }
                           });
        if (!taken) {
            ++rest_sends;
        }
    }

    TestClock clock{k_now, MonoNanos{1'000 * k_ms}};
    exec::InstrumentTable table{table_with_btc()};
    OrderCmd cmd{limit_buy(table)};
    net::Signer signer;
    RequestBuilder rest{signer};
    TradeFrameBuilder ws;
    TradeCodec codec{signer, "pass", k_no_offset};
    Sink sink;
    std::vector<std::string> socket;
    bool socket_up{true};
    int rest_sends{0};
    net::WsRpc rpc{clock, config(), codec, [this](std::string_view frame) {
                       if (!socket_up) {
                           return false;
                       }
                       socket.emplace_back(frame);
                       return true;
                   }};
};

TEST_F(OkxTradeRoute, BeforeTheLoginReplyTheOrderGoesOverRestOnly) {
    rpc.start();
    rpc.on_open();
    route();
    EXPECT_EQ(rest_sends, 1);
    EXPECT_EQ(socket.size(), 1U);
}

TEST_F(OkxTradeRoute, ALoggedInSocketTakesTheOrderAndRestNeverSeesIt) {
    log_in();
    route();
    EXPECT_EQ(rest_sends, 0);
    ASSERT_EQ(socket.size(), 1U);
    EXPECT_NE(socket[0].find(R"("clOrdId":"01ARZ3NDEKTSV4RRFFQ69G5FAV")"), std::string::npos);
}

TEST_F(OkxTradeRoute, AnInstrumentWithoutACodeGoesOverRest) {
    log_in();
    route(0);
    EXPECT_EQ(rest_sends, 1);
    EXPECT_TRUE(socket.empty());
}

TEST_F(OkxTradeRoute, ARefusedWriteGoesToRestAndLeavesNothingToResend) {
    log_in();
    socket_up = false;
    route();
    EXPECT_EQ(rest_sends, 1);
    EXPECT_EQ(rpc.in_flight(), 0U);
    socket_up = true;
    rpc.on_close(net::WsCloseReason::Transport);
    rpc.on_open();
    ASSERT_EQ(socket.size(), 1U);
    EXPECT_TRUE(socket[0].starts_with(R"({"op":"login")"));
    EXPECT_TRUE(sink.events.empty());
}

TEST_F(OkxTradeRoute, ATimeoutEndsUnknownAsAReconGapAndALateReplyChangesNothing) {
    log_in();
    route();
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

    auto late = fixture("ws_trade_order.json");
    late.replace(late.find("1512"), 4, "1");
    rpc.on_message(late, {});
    EXPECT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(rpc.stats().unmatched, 1U);
}

TEST_F(OkxTradeRoute, ADropMidFlightEndsUnknownAndTheNextSocketCarriesNoResend) {
    log_in();
    route();
    ASSERT_EQ(rpc.in_flight(), 1U);
    rpc.on_close(net::WsCloseReason::Transport);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::ReconGap);
    EXPECT_EQ(rpc.in_flight(), 0U);

    route();
    EXPECT_EQ(rest_sends, 1);
    socket.clear();
    rpc.on_open();
    ASSERT_EQ(socket.size(), 1U);
    EXPECT_TRUE(socket[0].starts_with(R"({"op":"login")"));
}

} // namespace
} // namespace raikiri::venues::okx
