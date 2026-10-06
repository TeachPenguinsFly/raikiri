#include <raikiri/venues/bybit/map.hpp>
#include <raikiri/venues/bybit/market_data.hpp>
#include <raikiri/venues/bybit/rest.hpp>
#include <raikiri/venues/bybit/trade.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

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

// a doc example under one of our numeric ids
std::string with_id(std::string frame, std::string_view id) {
    const auto start = frame.find(R"("reqId":")") + 9;
    const auto end = frame.find('"', start);
    frame.replace(start, end - start, id);
    return frame;
}

void load_credentials(net::Signer& signer) {
    signer.set_api_key("TESTKEY123");
    ASSERT_TRUE(signer.secret().assign("testsecret").has_value());
}

exec::InstrumentTable make_table() {
    exec::InstrumentTable table;
    table.reserve(1);
    WireInstrument btc{};
    btc.base_coin = "BTC";
    btc.quote_coin = "USDT";
    btc.symbol = "BTCUSDT";
    btc.status = "Trading";
    btc.tick_size = "0.10";
    btc.qty_step = "0.001";
    const auto instrument = map_instrument(btc, MarketType::Perp);
    EXPECT_TRUE(instrument.has_value());
    EXPECT_TRUE(table.add(*instrument).has_value());
    return table;
}

OrderCmd make_order(const exec::InstrumentTable& table) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.instrument = base_of(*table.find(std::string_view{"BTCUSDT"})).id;
    cmd.side = Side::Buy;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.flags = OrderFlags::PostOnly;
    cmd.price = PriceTicks::from_raw(160'000);
    cmd.qty = QtyLots::from_raw(3);
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("01ARZ3NDEKTSV4RRFFQ69G5FAV");
    return cmd;
}

TEST(BybitTrade, AnOrderFrameCarriesTheRestBodyAsItsOnlyArg) {
    const auto table = make_table();
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder rest{signer};
    const auto request = rest.create_order(make_order(table), table, k_now);
    ASSERT_TRUE(request.has_value());

    TradeFrameBuilder ws;
    const auto frame = ws.request(17, k_op_create, request->body, k_now);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(*frame,
              R"({"reqId":"17","header":{"X-BAPI-TIMESTAMP":"1700000000123","X-BAPI-RECV-WINDOW":"5000"},)"
              R"("op":"order.create","args":[)" +
                  std::string{request->body} + "]}");
    EXPECT_NE(frame->find(R"("orderLinkId":"01ARZ3NDEKTSV4RRFFQ69G5FAV")"), std::string_view::npos);
    EXPECT_NE(frame->find(R"("timeInForce":"PostOnly")"), std::string_view::npos);
}

TEST(BybitTrade, CancelAndAmendUseTheirOwnOps) {
    const auto table = make_table();
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder rest{signer};
    TradeFrameBuilder ws;
    const auto cancel = rest.cancel_order(make_order(table), table, k_now);
    ASSERT_TRUE(cancel.has_value());
    const auto cancel_frame = ws.request(18, k_op_cancel, cancel->body, k_now);
    ASSERT_TRUE(cancel_frame.has_value());
    EXPECT_NE(cancel_frame->find(R"("op":"order.cancel")"), std::string_view::npos);

    const auto amend = rest.amend_order(make_order(table), table, k_now);
    ASSERT_TRUE(amend.has_value());
    const auto amend_frame = ws.request(19, k_op_amend, amend->body, k_now);
    ASSERT_TRUE(amend_frame.has_value());
    EXPECT_NE(amend_frame->find(R"("op":"order.amend")"), std::string_view::npos);
}

TEST(BybitTrade, LoginIsTheSignedAuthOpAndNeedsAKey) {
    net::Signer signer;
    TradeCodec without{signer};
    EXPECT_FALSE(without.login(k_now).has_value());

    load_credentials(signer);
    TradeCodec codec{signer};
    const auto frame = codec.login(k_now);
    ASSERT_TRUE(frame.has_value());
    EXPECT_TRUE(frame->starts_with(R"({"op":"auth","args":["TESTKEY123",)"));
}

TEST(BybitTrade, DocumentedAuthRepliesSettleTheLogin) {
    net::Signer signer;
    TradeCodec codec{signer};
    const auto ok = codec.classify(fixture("ws_trade_auth.json"));
    EXPECT_EQ(ok.kind, net::WsRpcFrame::Kind::Login);
    EXPECT_TRUE(ok.ok);
    const auto failure = fixture("ws_trade_auth_failure.json");
    const auto refused = codec.classify(failure);
    EXPECT_EQ(refused.kind, net::WsRpcFrame::Kind::Login);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.detail, "Invalid sign");
    EXPECT_EQ(classify(k_ret_sign_error), exec::ErrorClass::AuthFailure);
}

TEST(BybitTrade, RepliesAreMatchedByReqIdAndKeepTheVenueCode) {
    net::Signer signer;
    TradeCodec codec{signer};
    EXPECT_EQ(codec.classify(fixture("ws_trade_create.json")).kind, net::WsRpcFrame::Kind::Other);

    const auto accepted = codec.classify(with_id(fixture("ws_trade_create.json"), "17"));
    EXPECT_EQ(accepted.kind, net::WsRpcFrame::Kind::Reply);
    EXPECT_EQ(accepted.id, 17U);
    EXPECT_TRUE(accepted.ok);

    const auto error = with_id(fixture("ws_trade_error.json"), "18");
    const auto refused = parse_trade_reply(error);
    ASSERT_TRUE(refused.has_value());
    EXPECT_EQ(refused->req_id, 18U);
    EXPECT_EQ(refused->op, "order.create");
    EXPECT_EQ(refused->ret_code, 10001);
    EXPECT_EQ(refused->ret_msg, "position idx not match position mode");
}

TEST(BybitTrade, ThePongIsTimed) {
    net::Signer signer;
    TradeCodec codec{signer};
    EXPECT_EQ(codec.classify(fixture("ws_trade_pong.json")).kind, net::WsRpcFrame::Kind::Pong);
    EXPECT_EQ(codec.ping(), R"({"op":"ping"})");
}

TEST(BybitTrade, NothingReachesTheSocketBeforeTheAuthReplyAndRestTakesItInstead) {
    net::Signer signer;
    load_credentials(signer);
    TradeCodec codec{signer};
    TestClock clock{k_now, MonoNanos{1}};
    std::vector<std::string> socket;
    net::WsRpc rpc{clock, net::WsRpcConfig{}, codec, [&](std::string_view frame) {
                       socket.emplace_back(frame);
                       return true;
                   }};
    rpc.start();
    rpc.on_open();
    ASSERT_EQ(socket.size(), 1U);
    int rest_sends = 0;
    if (!rpc.request(rpc.next_id(), "order", [](const net::WsRpcReply&) {})) {
        ++rest_sends;
    }
    EXPECT_EQ(rest_sends, 1);
    EXPECT_EQ(socket.size(), 1U);

    rpc.on_message(fixture("ws_trade_auth.json"), {});
    ASSERT_TRUE(rpc.ready());
    EXPECT_TRUE(rpc.request(rpc.next_id(), "order", [](const net::WsRpcReply&) {}));
    EXPECT_EQ(socket.size(), 2U);
}

TEST(BybitTickers, RestSpotTickersStampEveryRowWithTheEnvelopeTime) {
    exec::InstrumentTable table;
    WireInstrument btc{};
    btc.base_coin = "BTC";
    btc.quote_coin = "USDT";
    btc.symbol = "BTCUSDT";
    btc.status = "Trading";
    btc.tick_size = "0.01";
    btc.qty_step = "0.000001";
    btc.min_notional = "5";
    const auto spot = map_instrument(btc, MarketType::Spot);
    ASSERT_TRUE(spot.has_value());
    ASSERT_TRUE(table.add(*spot).has_value());

    const auto body = fixture("rest_tickers_spot.json");
    const auto storage = std::make_unique<TickerBuffer>();
    const auto rows = parse_tickers(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 5U);
    std::size_t mapped = 0;
    for (const auto& row : *rows) {
        EXPECT_EQ(row.ts_ms, 1'791'299'350'363ULL);
        const auto stats = map_ticker_stats(row, table, k_now);
        if (!stats.has_value()) {
            continue;
        }
        ++mapped;
        EXPECT_EQ(stats->volume_24h_base.raw, 5'184'551'794'000LL);
        EXPECT_EQ(stats->volume_24h_quote.raw, 445'000'078'465'471LL);
        EXPECT_EQ(stats->ts_event.ns, 1'791'299'350'363'000'000ULL);
    }
    EXPECT_EQ(mapped, 1U);
}

} // namespace
} // namespace raikiri::venues::bybit
