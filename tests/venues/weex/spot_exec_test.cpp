#include <raikiri/exec/order_gate.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/weex/catalogue.hpp>
#include <raikiri/venues/weex/endpoints.hpp>
#include <raikiri/venues/weex/envelope.hpp>
#include <raikiri/venues/weex/errors.hpp>
#include <raikiri/venues/weex/map.hpp>
#include <raikiri/venues/weex/orders.hpp>
#include <raikiri/venues/weex/rest.hpp>
#include <raikiri/venues/weex/spot.hpp>
#include <raikiri/venues/weex/ws.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::venues::weex {
namespace {

const std::string& fixture(std::string_view name) {
    static std::map<std::string, std::string, std::less<>> held;
    if (const auto found = held.find(name); found != held.end()) {
        return found->second;
    }
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    auto text = buf.str();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return held.emplace(std::string{name}, std::move(text)).first->second;
}

constexpr std::uint64_t k_ms = 1'000'000ULL;
constexpr UnixNanos k_now{1'764'506'000'000'000'000ULL};
constexpr std::string_view k_ulid = "01J8ZQ7X4K9M2P5R6T7V8W9XYZ";
constexpr std::string_view k_passphrase = "weextestpass";

class CaptureSink final : public exec::GatewaySink {
public:
    void on_bbo(const Bbo&) noexcept override {}

    void on_trade(const TradeTick&) noexcept override {}

    void on_funding(const FundingUpdate&) noexcept override {}

    void on_order_event(const OrderEvent& event) noexcept override { events.push_back(event); }

    void on_order_error(ClientOrderId, const exec::VenueError& error) noexcept override { errors.push_back(error); }

    void on_connectivity(const exec::ConnEvent& event) noexcept override { links.push_back(event.state); }

    std::vector<OrderEvent> events;
    std::vector<exec::VenueError> errors;
    std::vector<exec::ConnState> links;
};

exec::InstrumentTable spot_table() {
    static thread_local SpotInstrumentBuffer storage;
    const auto rows = parse_spot_exchange_info(fixture("spot_rest_exchange_info.json"), storage);
    EXPECT_TRUE(rows.has_value());
    exec::InstrumentTable table;
    for (const auto& row : *rows) {
        if (auto instrument = map_spot_instrument(row); instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value()) << row.symbol;
        }
    }
    return table;
}

const InstrumentBase& listed(const exec::InstrumentTable& table, std::string_view symbol) {
    const auto* instrument = table.find(symbol);
    EXPECT_NE(instrument, nullptr) << symbol;
    return base_of(*instrument);
}

struct TestSigner {
    net::Signer signer;

    TestSigner() {
        signer.set_api_key("weex-test-key");
        EXPECT_TRUE(signer.secret().assign("testsecret").has_value());
    }
};

std::string_view header_of(const net::RestRequest& request, std::string_view name) {
    for (const auto& header : request.headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return {};
}

// 0.0002 BTC at 68900.00 against a 0.000001 lot and a 0.01 tick
OrderCmd spot_limit(const exec::InstrumentTable& table) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.instrument = listed(table, "BTCUSDT").id;
    cmd.side = Side::Buy;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.client_order_id = ClientOrderId::make(1, 7);
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from(k_ulid);
    cmd.qty = QtyLots::from_raw(200);
    cmd.price = PriceTicks::from_raw(6'890'000);
    return cmd;
}

TEST(WeexSpotSign, ThePlaceOrderExampleSignsTheDocumentedStringAndItsHmac) {
    TestSigner credentials;
    const auto table = spot_table();
    RequestBuilder builder{credentials.signer, k_passphrase};
    auto cmd = spot_limit(table);
    cmd.qty = QtyLots::from_raw(1'000'000);
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("my-order-001");
    const auto request = builder.spot_create_order(cmd, table, UnixNanos{1'561'022'985'382'000'000ULL});
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(builder.last_sign_message(),
              R"(1561022985382POST/api/v3/order{"symbol":"BTCUSDT","side":"BUY","type":"LIMIT","timeInForce":"GTC",)"
              R"("quantity":"1","price":"68900","newClientOrderId":"my-order-001"})");
    EXPECT_EQ(header_of(*request, "ACCESS-SIGN"), "6Pcl0ssMOT4XEDJ5rFG5U6TAdiYIK96/G/Jj9pJ1Uf8=");
    EXPECT_EQ(header_of(*request, "ACCESS-TIMESTAMP"), "1561022985382");
    EXPECT_EQ(header_of(*request, "ACCESS-PASSPHRASE"), k_passphrase);
    EXPECT_EQ(header_of(*request, "ACCESS-KEY"), "weex-test-key");
    EXPECT_EQ(request->target, ep::k_spot_order);
    EXPECT_EQ(request->content_type, "application/json");
}

TEST(WeexSpotSign, TheSocketSignsTimestampAndThePrivatePath) {
    TestSigner credentials;
    const auto headers = spot_stream_headers(credentials.signer, k_passphrase, UnixNanos{1'561'022'985'382'000'000ULL});
    ASSERT_EQ(headers.size(), 4U);
    std::map<std::string, std::string> by_name{headers.begin(), headers.end()};
    EXPECT_EQ(by_name["ACCESS-TIMESTAMP"], "1561022985382");
    EXPECT_EQ(by_name["ACCESS-SIGN"], "Yy5zhfpDWZfl0ZzeP+4+rtC2jkfO9+HsErsjVrTUT7k=");
    EXPECT_EQ(by_name["ACCESS-KEY"], "weex-test-key");
    EXPECT_EQ(by_name["ACCESS-PASSPHRASE"], k_passphrase);
    EXPECT_TRUE(spot_stream_headers(credentials.signer, {}, k_now).empty());
    EXPECT_TRUE(spot_stream_headers(net::Signer{}, k_passphrase, k_now).empty());
}

TEST(WeexSpotSign, AGetWithoutAQuerySignsNoQuestionMark) {
    TestSigner credentials;
    RequestBuilder builder{credentials.signer, k_passphrase};
    ASSERT_TRUE(builder.spot_open_orders(UnixNanos{1'591'089'508'404'000'000ULL}).has_value());
    EXPECT_EQ(builder.last_sign_message(), "1591089508404GET/api/v3/openOrders");
    ASSERT_TRUE(builder.spot_account(UnixNanos{1'591'089'508'404'000'000ULL}).has_value());
    EXPECT_EQ(builder.last_sign_message(), "1591089508404GET/api/v3/account");
}

TEST(WeexSpotBuild, ACancelNamesThePairAndOurClientIdFirst) {
    TestSigner credentials;
    const auto table = spot_table();
    RequestBuilder builder{credentials.signer, k_passphrase};
    auto cmd = spot_limit(table);
    cmd.kind = CmdKind::Cancel;
    cmd.target_exchange_order_id = *ExchangeOrderId::from("702345678901234567");
    const auto by_client = builder.spot_cancel_order(cmd, table, k_now);
    ASSERT_TRUE(by_client.has_value());
    EXPECT_EQ(by_client->method, net::HttpMethod::Delete);
    EXPECT_EQ(by_client->target, "/api/v3/order?symbol=BTCUSDT&origClientOrderId=01J8ZQ7X4K9M2P5R6T7V8W9XYZ");
    EXPECT_TRUE(by_client->body.empty());

    cmd.exchange_client_order_id = {};
    const auto by_venue = builder.spot_cancel_order(cmd, table, k_now);
    ASSERT_TRUE(by_venue.has_value());
    EXPECT_EQ(by_venue->target, "/api/v3/order?symbol=BTCUSDT&orderId=702345678901234567");

    cmd.target_exchange_order_id = {};
    EXPECT_EQ(builder.spot_cancel_order(cmd, table, k_now).error(), BuildError::NoOrderTarget);
}

TEST(WeexSpotBuild, OnlyAPlainLimitLeavesTheAdapter) {
    TestSigner credentials;
    const auto table = spot_table();
    RequestBuilder builder{credentials.signer, k_passphrase};
    auto cmd = spot_limit(table);
    cmd.tif = TimeInForce::Ioc;
    ASSERT_TRUE(builder.spot_create_order(cmd, table, k_now).has_value());
    EXPECT_NE(builder.last_sign_message().find(R"("timeInForce":"IOC")"), std::string_view::npos);

    auto post_only = spot_limit(table);
    post_only.flags = OrderFlags::PostOnly;
    EXPECT_EQ(builder.spot_create_order(post_only, table, k_now).error(), BuildError::UnsupportedOrder);
    auto reduce_only = spot_limit(table);
    reduce_only.flags = OrderFlags::ReduceOnly;
    EXPECT_EQ(builder.spot_create_order(reduce_only, table, k_now).error(), BuildError::UnsupportedOrder);
    auto market = spot_limit(table);
    market.order_type = OrderType::Market;
    EXPECT_EQ(builder.spot_create_order(market, table, k_now).error(), BuildError::UnsupportedOrder);

    auto perp = spot_limit(table);
    perp.instrument = InstrumentId::make(VenueId::Weex, MarketType::Perp, SymbolId{1});
    EXPECT_EQ(builder.spot_create_order(perp, table, k_now).error(), BuildError::UnsupportedMarket);
    RequestBuilder unsigned_builder;
    EXPECT_EQ(unsigned_builder.spot_create_order(spot_limit(table), table, k_now).error(), BuildError::NotSigned);
}

TEST(WeexSpotBuild, TheFillWindowClampsToNinetyDaysAndNamesOneSymbol) {
    TestSigner credentials;
    RequestBuilder builder{credentials.signer, k_passphrase};
    const auto now_ms = k_now.ns / k_ms;
    const auto request = builder.spot_fills("BTCUSDT", UnixNanos{1}, k_now);
    ASSERT_TRUE(request.has_value());
    const auto start_ms = now_ms - 90ULL * 24 * 60 * 60 * 1000;
    EXPECT_EQ(request->target,
              "/api/v3/myTrades?symbol=BTCUSDT&limit=200&startTime=" + std::to_string(start_ms) +
                  "&endTime=" + std::to_string(now_ms));
    EXPECT_EQ(builder.spot_fills({}, UnixNanos{1}, k_now).error(), BuildError::UnknownInstrument);
    const auto query = builder.spot_query_order(k_ulid, k_now);
    ASSERT_TRUE(query.has_value());
    EXPECT_EQ(query->target, "/api/v3/order?origClientOrderId=01J8ZQ7X4K9M2P5R6T7V8W9XYZ");
}

TEST(WeexSpotParse, OpenOrdersCarryPartialFillsAsLeaves) {
    const auto table = spot_table();
    OrderBuffer storage;
    const auto rows = parse_orders(fixture("spot_rest_open_orders.json"), storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 2U);
    const auto first = map_open_order((*rows)[0], table);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->exchange_client_order_id.view(), k_ulid);
    EXPECT_EQ(first->qty.raw, 10'000);
    EXPECT_EQ(first->leaves.raw, 10'000);
    EXPECT_EQ(first->status, OrderStatus::Accepted);
    EXPECT_EQ(first->price.raw, 6'890'000);
    const auto second = map_open_order((*rows)[1], table);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->side, Side::Sell);
    EXPECT_EQ(second->status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(second->leaves.raw, 120);
    EXPECT_EQ(second->exchange_client_order_id.view(), "my-spot-order-002");
}

TEST(WeexSpotParse, OneOrderReadsLikeAnOpenRow) {
    const auto table = spot_table();
    const auto wire = parse_order(fixture("spot_rest_order.json"));
    ASSERT_TRUE(wire.has_value());
    const auto report = map_open_order(*wire, table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->status, OrderStatus::Filled);
    EXPECT_EQ(report->leaves.raw, 0);
    EXPECT_EQ(report->exchange_order_id.view(), "702345678901234567");
}

TEST(WeexSpotParse, TradesTakeSideFromIsBuyerAndTheirCommissionAsGiven) {
    const auto table = spot_table();
    SpotTradeBuffer storage;
    const auto rows = parse_spot_trades(fixture("spot_rest_my_trades.json"), storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 2U);
    const auto buy = map_spot_trade((*rows)[0], table);
    ASSERT_TRUE(buy.has_value());
    EXPECT_EQ(buy->side, Side::Buy);
    EXPECT_EQ(buy->price.raw, 6'895'000);
    EXPECT_EQ(buy->qty.raw, 10'000);
    EXPECT_EQ(buy->fee.raw, 138'000'000);
    EXPECT_EQ(buy->exec_id.view(), "801234567890123456");
    EXPECT_EQ(buy->exchange_order_id.view(), "702345678901234567");
    EXPECT_EQ(buy->liquidity, Liquidity::Unknown);
    EXPECT_EQ(buy->ts_venue.ns, 1'764'506'001'556ULL * k_ms);
    const auto sell = map_spot_trade((*rows)[1], table);
    ASSERT_TRUE(sell.has_value());
    EXPECT_EQ(sell->side, Side::Sell);
}

TEST(WeexSpotParse, TheAccountTotalIsFreePlusLocked) {
    SpotBalanceBuffer storage;
    const auto rows = parse_spot_account(fixture("spot_rest_account.json"), storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 2U);
    const auto usdt = map_spot_balance((*rows)[1], k_now);
    ASSERT_TRUE(usdt.has_value());
    EXPECT_EQ(usdt->asset.view(), "USDT");
    EXPECT_EQ(usdt->total.raw, 1'200'000'000'000);
    EXPECT_EQ(usdt->available.raw, 1'200'000'000'000);
    EXPECT_TRUE(usdt->unrealized.is_undef());
}

TEST(WeexSpotParse, TheTradableListIsTheContractListsShape) {
    TradableBuffer storage;
    const auto names = parse_api_trading_symbols(fixture("spot_rest_api_trading_symbols.json"), storage);
    ASSERT_TRUE(names.has_value());
    ASSERT_EQ(names->size(), 3U);
    EXPECT_EQ((*names)[0], "BTCUSDT");
}

TEST(WeexSpotVerdict, AnAckIsAcceptedWithTheVenueId) {
    const auto placed = spot_verdict(false, 200, fixture("spot_rest_order_ack.json"));
    EXPECT_EQ(placed.kind, SpotVerdictKind::Accepted);
    EXPECT_EQ(placed.order_id, "702345678901234567");
    const auto canceled = spot_verdict(true, 200, fixture("spot_rest_cancel_ack.json"));
    EXPECT_EQ(canceled.kind, SpotVerdictKind::Canceled);
    EXPECT_EQ(canceled.order_id, "702345678901234567");
}

TEST(WeexSpotVerdict, RefusalsCarryTheirClass) {
    const auto auth = spot_verdict(false, 401, fixture("spot_rest_error_unauthorized.json"));
    EXPECT_EQ(auth.kind, SpotVerdictKind::Refused);
    EXPECT_EQ(auth.error.cls, exec::ErrorClass::AuthFailure);
    EXPECT_EQ(auth.error.venue_code, -1044);
    const auto body = spot_verdict(false, 400, fixture("spot_rest_error_bad_body.json"));
    EXPECT_EQ(body.kind, SpotVerdictKind::Refused);
    EXPECT_EQ(body.error.cls, exec::ErrorClass::Terminal);
    const auto limited = spot_verdict(false, 429, "");
    EXPECT_EQ(limited.kind, SpotVerdictKind::Refused);
    EXPECT_EQ(limited.error.cls, exec::ErrorClass::RateLimited);
    const auto unrouted = spot_verdict(false, 404, R"({"status":404,"error":"Not Found"})");
    EXPECT_EQ(unrouted.kind, SpotVerdictKind::Refused);
    EXPECT_EQ(unrouted.error.cls, exec::ErrorClass::Terminal);
    const auto not_tradable = spot_verdict(false, 400, R"({"code":-1058,"msg":"not via api"})");
    EXPECT_EQ(not_tradable.error.cls, exec::ErrorClass::Terminal);
    const auto too_fast = spot_verdict(false, 200, R"({"code":-1059,"msg":"too many"})");
    EXPECT_EQ(too_fast.error.cls, exec::ErrorClass::RateLimited);
}

TEST(WeexSpotVerdict, ACreateTheVenueMayHaveTakenIsUnknownNeverRefused) {
    for (const auto& [status, body] : std::vector<std::pair<int, std::string>>{
             {500, fixture("spot_rest_error_unknown.json")},
             {200, R"({"code":-1054,"msg":"System error, please retry later."})"},
             {502, ""},
             {200, "<html>gateway</html>"},
             {200, R"({"symbol":"BTCUSDT"})"},
         }) {
        const auto verdict = spot_verdict(false, status, body);
        EXPECT_EQ(verdict.kind, SpotVerdictKind::Unknown) << status << " " << body;
        EXPECT_EQ(verdict.error.cls, exec::ErrorClass::Retryable) << body;
    }
    // a cancel in doubt leaves the order as it was; reconciliation settles it
    EXPECT_EQ(spot_verdict(true, 500, fixture("spot_rest_error_unknown.json")).kind, SpotVerdictKind::Refused);
}

TEST(WeexSpotVerdict, ACancelOfAGoneOrderIsCanceledAndOfAFilledOneIsRefused) {
    const auto gone = spot_verdict(true, 400, R"({"code":-2200,"msg":"Order does not exist."})");
    EXPECT_EQ(gone.kind, SpotVerdictKind::Canceled);
    const auto filled = spot_verdict(true, 400, R"({"code":-2202,"msg":"Order is fully filled."})");
    EXPECT_EQ(filled.kind, SpotVerdictKind::Refused);
    EXPECT_EQ(filled.error.venue_code, k_spot_order_filled);
    EXPECT_EQ(spot_verdict(false, 400, R"({"code":-2200,"msg":"x"})").kind, SpotVerdictKind::Refused);
}

TEST(WeexSpotFeed, PrivateFramesAreRecognised) {
    EXPECT_EQ(parse_spot_private(fixture("spot_ws_private_orders.json"))->kind, SpotPrivateKind::Orders);
    EXPECT_EQ(parse_spot_private(fixture("spot_ws_private_fill.json"))->kind, SpotPrivateKind::Fill);
    EXPECT_EQ(parse_spot_private(fixture("spot_ws_private_account.json"))->kind, SpotPrivateKind::Account);
    EXPECT_EQ(parse_spot_private(fixture("spot_ws_private_ping.json"))->kind, SpotPrivateKind::Ping);
    EXPECT_EQ(parse_spot_private(fixture("spot_ws_private_ack.json"))->kind, SpotPrivateKind::Ack);
    const auto refused = parse_spot_private(R"({"result":false,"id":2,"msg":"auth failed"})");
    ASSERT_TRUE(refused.has_value());
    EXPECT_EQ(refused->kind, SpotPrivateKind::Refusal);
    EXPECT_EQ(refused->msg, "auth failed");
}

class WeexSpotRoute : public ::testing::Test {
protected:
    // the adapter's create path: build, record in flight, then wait for rest or the socket
    void send() {
        const auto request = rest.spot_create_order(cmd, table, clock.wall());
        ASSERT_TRUE(request.has_value());
        ASSERT_TRUE(in_flight.add(exec::in_flight_of(cmd, clock.mono())));
        feed.note_symbol(table.exchange_symbol(cmd.instrument));
    }

    void answer(int status, std::string_view body) {
        (void)in_flight.resolve(cmd.client_order_id);
        const auto verdict = spot_verdict(false, status, body);
        if (verdict.kind == SpotVerdictKind::Accepted) {
            feed.remember(verdict.order_id,
                          {.client_order_id = cmd.exchange_client_order_id,
                           .instrument = cmd.instrument,
                           .side = cmd.side,
                           .qty = cmd.qty,
                           .filled = QtyLots::from_raw(0)});
        }
        if (verdict.kind == SpotVerdictKind::Unknown) {
            exec::report_unknown_outcome(
                sink, exec::in_flight_of(cmd, clock.mono()), clock.wall(), verdict.error.message.view());
        }
    }

    void sweep() {
        for (const auto& stale : in_flight.expire(clock.mono(), k_timeout_ns)) {
            exec::report_unknown_outcome(sink, stale, clock.wall(), "no answer");
        }
    }

    // what the socket handler sends on open: the three subscribes and nothing else
    std::vector<std::string_view> reopen() {
        return {k_spot_private_subscribes.begin(), k_spot_private_subscribes.end()};
    }

    static constexpr std::uint64_t k_timeout_ns = 15'000 * k_ms;

    TestSigner credentials;
    TestClock clock{k_now, MonoNanos{1'000 * k_ms}};
    exec::InstrumentTable table{spot_table()};
    OrderCmd cmd{[this] {
        auto order = spot_limit(table);
        order.qty = QtyLots::from_raw(952);
        order.price = PriceTicks::from_raw(10'500'800);
        return order;
    }()};
    RequestBuilder rest{credentials.signer, k_passphrase};
    CaptureSink sink;
    SpotPrivateFeed feed{sink, table, clock};
    exec::InFlightTable in_flight{16};
};

TEST_F(WeexSpotRoute, ADropBeforeTheAnswerEndsAsAGapAndTheNextSocketResendsNothing) {
    send();
    feed.on_close();
    ASSERT_EQ(sink.links.size(), 1U);
    EXPECT_EQ(sink.links[0], exec::ConnState::Disconnected);

    clock.advance(k_timeout_ns - k_ms);
    sweep();
    EXPECT_TRUE(sink.events.empty());
    clock.advance(2 * k_ms);
    sweep();
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::ReconGap);
    EXPECT_EQ(sink.events[0].client_order_id, cmd.client_order_id);
    EXPECT_TRUE(sink.events[0].leaves_qty.is_undef());

    for (const auto frame : reopen()) {
        EXPECT_EQ(frame.find("newClientOrderId"), std::string_view::npos);
        EXPECT_EQ(parse_spot_private(frame)->kind, SpotPrivateKind::Unknown);
    }
    for (std::size_t i = 0; i < k_spot_private_subscribes.size(); ++i) {
        EXPECT_FALSE(feed.on_frame(fixture("spot_ws_private_ack.json"), k_now).has_value());
    }
    EXPECT_EQ(sink.links.back(), exec::ConnState::Connected);

    // the push after the reconnect is what settles the gap
    EXPECT_FALSE(feed.on_frame(fixture("spot_ws_private_orders_new.json"), k_now).has_value());
    ASSERT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(sink.events[1].type, OrderEventType::Accepted);
    EXPECT_EQ(sink.events[1].exchange_client_order_id.view(), k_ulid);
    EXPECT_EQ(sink.events[1].leaves_qty.raw, 952);
    EXPECT_EQ(sink.events[1].price.raw, 10'500'800);

    EXPECT_FALSE(feed.on_frame(fixture("spot_ws_private_fill.json"), k_now).has_value());
    ASSERT_EQ(sink.events.size(), 3U);
    const auto& fill = sink.events[2];
    EXPECT_EQ(fill.type, OrderEventType::Filled);
    EXPECT_EQ(fill.exchange_client_order_id.view(), k_ulid);
    EXPECT_EQ(fill.exec_id.view(), "7423916138");
    EXPECT_EQ(fill.price.raw, 10'500'800);
    EXPECT_EQ(fill.qty.raw, 952);
    EXPECT_EQ(fill.leaves_qty.raw, 0);
    EXPECT_EQ(fill.liquidity, Liquidity::Taker);
    // 0.00000095 BTC at 105008.00 is 0.0997576 USDT
    EXPECT_EQ(fill.fee.raw, 99'757'600);
    EXPECT_EQ(fill.ts_event.ns, 1'749'044'695'718ULL * k_ms);
    EXPECT_EQ(feed.contexts(), 0U);
}

TEST_F(WeexSpotRoute, AFiveHundredIsAGapAtOnceNeverAReject) {
    send();
    answer(500, fixture("spot_rest_error_unknown.json"));
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::ReconGap);
    EXPECT_EQ(in_flight.size(), 0U);
    clock.advance(2 * k_timeout_ns);
    sweep();
    EXPECT_EQ(sink.events.size(), 1U);
}

TEST_F(WeexSpotRoute, AFillBeforeItsOrderPushCountsOnce) {
    send();
    answer(200, R"({"symbol":"BTCUSDT","orderId":"625138763307155610","clientOrderId":"01J8ZQ7X4K9M2P5R6T7V8W9XYZ"})");
    EXPECT_TRUE(sink.events.empty());
    (void)feed.on_frame(fixture("spot_ws_private_fill.json"), k_now);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].leaves_qty.raw, 0);
    EXPECT_EQ(sink.events[0].exchange_client_order_id.view(), k_ulid);
    // the documented FILLED push adds nothing; fills come from the fill channel only
    (void)feed.on_frame(fixture("spot_ws_private_orders.json"), k_now);
    EXPECT_EQ(sink.events.size(), 1U);
}

TEST_F(WeexSpotRoute, APingIsAnsweredAndARefusedSubscribeFailsTheLink) {
    const auto pong = feed.on_frame(fixture("spot_ws_private_ping.json"), k_now);
    ASSERT_TRUE(pong.has_value());
    EXPECT_EQ(*pong, k_pong_frame);
    (void)feed.on_frame(R"({"result":false,"id":1,"msg":"auth failed"})", k_now);
    ASSERT_EQ(sink.links.size(), 1U);
    EXPECT_EQ(sink.links[0], exec::ConnState::AuthFailed);
    (void)feed.on_frame(fixture("spot_ws_private_account.json"), k_now);
    EXPECT_TRUE(sink.events.empty());
    EXPECT_TRUE(feed.symbols().empty());
}

TEST_F(WeexSpotRoute, EverySymbolTradedOrSeenIsSweptForFills) {
    send();
    (void)feed.on_frame(fixture("spot_ws_private_orders.json"), k_now);
    OrderBuffer storage;
    const auto rows = parse_orders(fixture("spot_rest_open_orders.json"), storage);
    ASSERT_TRUE(rows.has_value());
    for (const auto& row : *rows) {
        feed.note_symbol(row.symbol);
    }
    EXPECT_EQ(feed.symbols().size(), 2U);
    EXPECT_TRUE(feed.symbols().contains("ETHBTC"));
}

} // namespace
} // namespace raikiri::venues::weex
