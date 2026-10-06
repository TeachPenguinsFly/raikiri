#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/gate/catalogue.hpp>
#include <raikiri/venues/gate/envelope.hpp>
#include <raikiri/venues/gate/errors.hpp>
#include <raikiri/venues/gate/map.hpp>
#include <raikiri/venues/gate/market_data.hpp>
#include <raikiri/venues/gate/orders.hpp>
#include <raikiri/venues/gate/rest.hpp>
#include <raikiri/venues/gate/trade.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::gate {
namespace {

constexpr UnixNanos k_now{1'700'000'000'123'000'000ULL};
constexpr std::uint64_t k_now_s = 1'700'000'000ULL;
constexpr std::uint64_t k_ms = 1'000'000ULL;
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
    table.reserve(1);
    WireContract wire{};
    wire.name = "BTC_USDT";
    wire.status = "trading";
    wire.order_price_round = "0.1";
    wire.order_size_min = "1";
    wire.quanto_multiplier = "0.0001";
    wire.funding_interval_s = 28'800;
    const auto instrument = map_instrument(wire, MarketType::Perp);
    EXPECT_TRUE(instrument.has_value());
    EXPECT_TRUE(table.add(*instrument).has_value());
    return table;
}

exec::InstrumentTable spot_table() {
    exec::InstrumentTable table;
    table.reserve(1);
    CurrencyPairBuffer storage;
    const auto body = fixture("rest_currency_pairs.json");
    const auto pairs = parse_currency_pairs(body, storage);
    EXPECT_TRUE(pairs.has_value());
    const auto spot = map_currency_pair((*pairs)[2]);
    EXPECT_TRUE(spot.has_value());
    EXPECT_TRUE(table.add(*spot).has_value());
    return table;
}

OrderCmd limit_buy(const exec::InstrumentTable& table) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.instrument = base_of(*table.find(std::string_view{"BTC_USDT"})).id;
    cmd.side = Side::Buy;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.price = PriceTicks::from_raw(640'000);
    cmd.qty = QtyLots::from_raw(10);
    cmd.client_order_id = ClientOrderId{77};
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("01J8ZQ7X4K9M2P5R6T7V8W9XYZ");
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

TEST(GateTrade, APlaceCarriesTheRestBodyAsItsReqParam) {
    const auto table = table_with_btc();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder rest{signer};
    const auto request = rest.create_order(limit_buy(table), table, k_now);
    ASSERT_TRUE(request.has_value());

    TradeFrameBuilder ws;
    const auto frame = ws.request(17, MarketType::Perp, TradeOp::Place, {}, {}, request->body, k_now_s);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(*frame,
              R"({"time":1700000000,"channel":"futures.order_place","event":"api","payload":{"req_id":"17",)"
              R"("req_param":)" +
                  std::string{request->body} + "}}");
    EXPECT_NE(frame->find(R"("text":"t-01J8ZQ7X4K9M2P5R6T7V8W9XYZ")"), std::string_view::npos);
}

TEST(GateTrade, CancelAndAmendNameTheOrderAndSpotAddsItsPair) {
    const auto table = table_with_btc();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder rest{signer};
    TradeFrameBuilder ws;
    std::array<char, k_client_id_bytes> buf{};
    const auto cmd = limit_buy(table);
    const auto target = order_target(cmd, buf);
    EXPECT_EQ(target, "t-01J8ZQ7X4K9M2P5R6T7V8W9XYZ");

    const auto cancel = ws.request(18, MarketType::Perp, TradeOp::Cancel, target, "BTC_USDT", {}, k_now_s);
    ASSERT_TRUE(cancel.has_value());
    EXPECT_NE(cancel->find(R"("channel":"futures.order_cancel")"), std::string_view::npos);
    EXPECT_NE(cancel->find(R"("req_param":{"order_id":"t-01J8ZQ7X4K9M2P5R6T7V8W9XYZ"}}})"), std::string_view::npos);

    const auto amend_body = rest.amend_order(cmd, table, k_now);
    ASSERT_TRUE(amend_body.has_value());
    const auto amend = ws.request(19, MarketType::Perp, TradeOp::Amend, target, "BTC_USDT", amend_body->body, k_now_s);
    ASSERT_TRUE(amend.has_value());
    EXPECT_NE(amend->find(R"("req_param":{"order_id":"t-01J8ZQ7X4K9M2P5R6T7V8W9XYZ","size":10,"price":"64000")"),
              std::string_view::npos);

    const auto spot = ws.request(20, MarketType::Spot, TradeOp::Cancel, target, "BTC_USDT", {}, k_now_s);
    ASSERT_TRUE(spot.has_value());
    EXPECT_NE(spot->find(R"("channel":"spot.order_cancel")"), std::string_view::npos);
    EXPECT_NE(spot->find(R"({"order_id":"t-01J8ZQ7X4K9M2P5R6T7V8W9XYZ","currency_pair":"BTC_USDT","account":"spot"})"),
              std::string_view::npos);
    EXPECT_FALSE(ws.request(21, MarketType::Spot, TradeOp::Cancel, target, {}, {}, k_now_s).has_value());
    EXPECT_FALSE(ws.request(22, MarketType::Future, TradeOp::Place, {}, {}, "{}", k_now_s).has_value());
}

TEST(GateTrade, ASpotPlaceGoesOnTheSpotChannel) {
    const auto table = spot_table();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder rest{signer};
    auto cmd = limit_buy(table);
    cmd.qty = QtyLots::from_raw(1);
    const auto request = rest.create_order(cmd, table, k_now);
    ASSERT_TRUE(request.has_value());
    TradeFrameBuilder ws;
    const auto frame = ws.request(23, MarketType::Spot, TradeOp::Place, {}, {}, request->body, k_now_s);
    ASSERT_TRUE(frame.has_value());
    EXPECT_NE(frame->find(R"("channel":"spot.order_place")"), std::string_view::npos);
    EXPECT_NE(frame->find(R"("currency_pair":"BTC_USDT")"), std::string_view::npos);
}

TEST(GateTrade, LoginSignsTheChannelWithAnEmptyParamAtVenueTime) {
    net::Signer signer;
    TradeCodec without{signer, MarketType::Perp, k_no_offset};
    EXPECT_FALSE(without.login(k_now).has_value());

    load_credentials(signer);
    const std::int64_t offset = 5'000'000'000LL;
    TradeCodec codec{signer, MarketType::Perp, offset};
    const auto frame = codec.login(k_now);
    ASSERT_TRUE(frame.has_value());
    EXPECT_TRUE(frame->starts_with(
        R"({"time":1700000005,"channel":"futures.login","event":"api","payload":{"api_key":"TESTKEY123","signature":")"));
    std::array<char, net::k_hex512_bytes> expected{};
    const auto signature = signer.sign("api\nfutures.login\n\n1700000005", net::Signer::Encoding::Hex512, expected);
    ASSERT_FALSE(signature.empty());
    EXPECT_NE(frame->find(signature), std::string::npos);
    EXPECT_TRUE(frame->ends_with(R"("timestamp":"1700000005","req_id":"0"}})"));

    TradeCodec spot{signer, MarketType::Spot, offset};
    EXPECT_NE(spot.login(k_now)->find(R"("channel":"spot.login")"), std::string::npos);
}

TEST(GateTrade, LoginRepliesSettleTheLogin) {
    net::Signer signer;
    TradeCodec codec{signer, MarketType::Perp, k_no_offset};
    const auto ok = codec.classify(fixture("ws_api_login.json"));
    EXPECT_EQ(ok.kind, net::WsRpcFrame::Kind::Login);
    EXPECT_TRUE(ok.ok);
    const auto failure = fixture("ws_api_login_failure.json");
    const auto refused = codec.classify(failure);
    EXPECT_EQ(refused.kind, net::WsRpcFrame::Kind::Login);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.detail, "INVALID_KEY");
}

TEST(GateTrade, CapturedRefusalsAreMatchedByRequestIdWithTheirLabel) {
    net::Signer signer;
    TradeCodec codec{signer, MarketType::Perp, k_no_offset};
    for (const auto* name : {"ws_api_not_login.json", "ws_api_spot_not_login.json"}) {
        const auto frame = fixture(name);
        const auto reply = codec.classify(frame);
        EXPECT_EQ(reply.kind, net::WsRpcFrame::Kind::Reply) << name;
        EXPECT_EQ(reply.id, 17U) << name;
        EXPECT_FALSE(reply.ok) << name;
        EXPECT_EQ(reply.detail, "AUTHENTICATION_FAILED") << name;
        const auto parsed = parse_trade_reply(frame);
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->status, 400U);
        EXPECT_EQ(parsed->message, "Not login");
        EXPECT_EQ(classify(parsed->label), exec::ErrorClass::AuthFailure);
    }
}

TEST(GateTrade, TheAckIsSkippedAndTheResultIsTheRestOrder) {
    net::Signer signer;
    TradeCodec codec{signer, MarketType::Perp, k_no_offset};
    EXPECT_EQ(codec.classify(fixture("ws_api_order_place_ack.json")).kind, net::WsRpcFrame::Kind::Other);

    const auto placed = fixture("ws_api_order_place.json");
    const auto reply = codec.classify(placed);
    EXPECT_EQ(reply.kind, net::WsRpcFrame::Kind::Reply);
    EXPECT_EQ(reply.id, 17U);
    EXPECT_TRUE(reply.ok);

    const auto parsed = parse_trade_reply(placed);
    ASSERT_TRUE(parsed.has_value());
    const auto over_ws = parse_rest_order(parsed->result, MarketType::Perp);
    const auto rest_body = fixture("rest_futures_order.json");
    const auto over_rest = parse_rest_order(rest_body, MarketType::Perp);
    ASSERT_TRUE(over_ws.has_value());
    ASSERT_TRUE(over_rest.has_value());
    EXPECT_EQ(over_ws->order_id, over_rest->order_id);
    EXPECT_EQ(over_ws->text, over_rest->text);
    EXPECT_EQ(over_ws->left, over_rest->left);
    EXPECT_EQ(over_ws->state, over_rest->state);
}

TEST(GateTrade, ACancelOfAGoneOrderCarriesTheBenignLabel) {
    const auto frame = fixture("ws_api_order_cancel_not_found.json");
    const auto parsed = parse_trade_reply(frame);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->id, 18U);
    EXPECT_EQ(parsed->status, 400U);
    EXPECT_TRUE(is_benign_cancel_failure(parsed->label));
}

class GateTradeRoute : public ::testing::Test {
protected:
    void SetUp() override { load_credentials(signer); }

    static net::WsRpcConfig config() {
        net::WsRpcConfig cfg{};
        cfg.request_timeout_ns = 500 * k_ms;
        return cfg;
    }

    void log_in() {
        rpc.start();
        rpc.on_open();
        ASSERT_EQ(socket.size(), 1U);
        rpc.on_message(fixture("ws_api_login.json"), {});
        ASSERT_TRUE(rpc.ready());
        socket.clear();
    }

    // the adapter's routing: rest runs only when the socket did not take the command
    void route() {
        const auto request = rest.create_order(cmd, table, k_now);
        ASSERT_TRUE(request.has_value());
        const auto id = rpc.next_id();
        const auto frame = ws.request(id, MarketType::Perp, TradeOp::Place, {}, {}, request->body, k_now_s);
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
    SignedRequestBuilder rest{signer};
    TradeFrameBuilder ws;
    TradeCodec codec{signer, MarketType::Perp, k_no_offset};
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

TEST_F(GateTradeRoute, BeforeTheLoginReplyTheOrderGoesOverRestOnly) {
    rpc.start();
    rpc.on_open();
    route();
    EXPECT_EQ(rest_sends, 1);
    EXPECT_EQ(socket.size(), 1U);
}

TEST_F(GateTradeRoute, ALoggedInSocketTakesTheOrderAndRestNeverSeesIt) {
    log_in();
    route();
    EXPECT_EQ(rest_sends, 0);
    ASSERT_EQ(socket.size(), 1U);
    EXPECT_NE(socket[0].find(R"("text":"t-01J8ZQ7X4K9M2P5R6T7V8W9XYZ")"), std::string::npos);
}

TEST_F(GateTradeRoute, TheAckDoesNotSettleTheRequest) {
    log_in();
    route();
    auto ack = fixture("ws_api_order_place_ack.json");
    ack.replace(ack.find(R"("request_id":"17")"), 17, R"("request_id":"1")");
    rpc.on_message(ack, {});
    EXPECT_EQ(rpc.in_flight(), 1U);
    auto placed = fixture("ws_api_order_place.json");
    placed.replace(placed.find(R"("request_id":"17")"), 17, R"("request_id":"1")");
    rpc.on_message(placed, {});
    EXPECT_EQ(rpc.in_flight(), 0U);
    EXPECT_TRUE(sink.events.empty());
}

TEST_F(GateTradeRoute, ARefusedWriteGoesToRestAndLeavesNothingToResend) {
    log_in();
    socket_up = false;
    route();
    EXPECT_EQ(rest_sends, 1);
    EXPECT_EQ(rpc.in_flight(), 0U);
    socket_up = true;
    rpc.on_close(net::WsCloseReason::Transport);
    rpc.on_open();
    ASSERT_EQ(socket.size(), 1U);
    EXPECT_NE(socket[0].find(R"("channel":"futures.login")"), std::string::npos);
    EXPECT_TRUE(sink.events.empty());
}

TEST_F(GateTradeRoute, ATimeoutEndsUnknownAsAReconGapAndALateReplyChangesNothing) {
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

    auto late = fixture("ws_api_order_place.json");
    late.replace(late.find(R"("request_id":"17")"), 17, R"("request_id":"1")");
    rpc.on_message(late, {});
    EXPECT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(rpc.stats().unmatched, 1U);
}

TEST_F(GateTradeRoute, ADropMidFlightEndsUnknownAndTheNextSocketCarriesNoResend) {
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
    EXPECT_NE(socket[0].find(R"("channel":"futures.login")"), std::string::npos);
}

TEST(GateSpotTickers, EveryPairInOneCallAndNoVenueStamp) {
    exec::InstrumentTable table;
    CurrencyPairBuffer pair_storage;
    const auto catalogue = fixture("rest_currency_pairs.json");
    const auto pairs = parse_currency_pairs(catalogue, pair_storage);
    ASSERT_TRUE(pairs.has_value());
    for (const auto& pair : *pairs) {
        if (const auto spot = map_currency_pair(pair); spot.has_value() && pair.id == "BTC_USDT") {
            ASSERT_TRUE(table.add(*spot).has_value());
        }
    }
    ASSERT_EQ(table.size(), 1U);

    const auto body = fixture("rest_spot_tickers.json");
    const auto storage = std::make_unique<SpotTickerBuffer>();
    const auto rows = parse_spot_tickers(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 5U);
    std::size_t mapped = 0;
    for (const auto& row : *rows) {
        const auto stats = map_ticker_stats(row, table, k_now);
        if (!stats.has_value()) {
            continue;
        }
        ++mapped;
        EXPECT_EQ(stats->volume_24h_base.raw, 5'954'783'550'000LL);
        EXPECT_EQ(stats->volume_24h_quote.raw, 511'140'793'095'910LL);
        EXPECT_EQ(stats->ts_event.ns, 0U) << "the venue does not stamp the row";
    }
    EXPECT_EQ(mapped, 1U);
}

} // namespace
} // namespace raikiri::venues::gate
