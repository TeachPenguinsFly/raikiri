#include <raikiri/common/time.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/venues/bitunix/bitunix_venue.hpp>
#include <raikiri/venues/bitunix/endpoints.hpp>
#include <raikiri/venues/bitunix/map.hpp>
#include <raikiri/venues/bitunix/orders.hpp>
#include <raikiri/venues/bitunix/trade.hpp>
#include <raikiri/venues/bitunix/wire.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::bitunix {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

constexpr UnixNanos k_ingest{1'790'964'213'000'000'000ULL};

FixedE9 e9(std::string_view text) {
    return parse_decimal_e9(text).value_or(FixedE9{});
}

WirePair pair(std::string_view symbol) {
    static const auto body = fixture("rest_trading_pairs.json");
    static const auto storage = std::make_unique<PairBuffer>();
    const auto rows = parse_pairs(body, *storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& one : *rows) {
        if (one.symbol == symbol) {
            return one;
        }
    }
    ADD_FAILURE() << "no pair " << symbol;
    return {};
}

InstrumentBase btc() {
    const auto instrument = map_pair(pair("BTCUSDT"), MarketType::Perp);
    EXPECT_TRUE(instrument.has_value());
    auto base = base_of(*instrument);
    base.id = InstrumentId::make(VenueId::Bitunix, MarketType::Perp, SymbolId{2});
    return base;
}

TEST(BitunixCatalogue, LoadsOnlyOpenLinearContractsTheApiTrades) {
    const auto body = fixture("rest_trading_pairs.json");
    const auto storage = std::make_unique<PairBuffer>();
    const auto rows = parse_pairs(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 7U);

    std::size_t loaded = 0;
    for (const auto& one : *rows) {
        loaded += map_pair(one, MarketType::Perp).has_value() ? 1U : 0U;
    }
    EXPECT_EQ(loaded, 4U);
    EXPECT_FALSE(pair("QQQUSDT").api);
    EXPECT_FALSE(map_pair(pair("QQQUSDT"), MarketType::Perp).has_value());
    EXPECT_FALSE(map_pair(pair("FDUSDUSDT"), MarketType::Perp).has_value()) << "PREVIEW";
    EXPECT_FALSE(map_pair(pair("BTCUSD"), MarketType::Perp).has_value()) << "coin-margined";

    const auto perp = map_pair(pair("BTCUSDT"), MarketType::Perp);
    ASSERT_TRUE(perp.has_value());
    const auto& base = base_of(*perp);
    EXPECT_EQ(base.exchange_symbol.view(), "BTCUSDT");
    EXPECT_EQ(base.local_symbol.view(), "BTC/USDT");
    EXPECT_EQ(base.tick_size, e9("0.1"));
    EXPECT_EQ(base.lot_size, e9("0.0001"));
    EXPECT_EQ(base.kind, ContractKind::Linear);
    const auto& row = std::get<PerpInstrument>(*perp);
    EXPECT_EQ(row.funding_cap, e9("0.003"));
    EXPECT_EQ(row.funding_floor, e9("-0.003"));
}

TEST(BitunixCatalogue, SpotLoadsOpenPairsOnly) {
    const auto body = fixture("spot_rest_coin_pairs.json");
    const auto status = parse_status(body);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->code, k_ok) << "the spot host states its code as a string";

    const auto storage = std::make_unique<PairBuffer>();
    const auto rows = parse_spot_pairs(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 3U);
    const auto spot = map_pair((*rows)[0], MarketType::Spot);
    ASSERT_TRUE(spot.has_value());
    EXPECT_TRUE(std::holds_alternative<SpotInstrument>(*spot));
    EXPECT_EQ(base_of(*spot).exchange_symbol.view(), "BTCUSDT");
    EXPECT_EQ(base_of(*spot).local_symbol.view(), "BTC/USDT");
    EXPECT_EQ(base_of(*spot).tick_size, e9("0.01"));
    EXPECT_EQ(base_of(*spot).lot_size, e9("0.00001"));
    EXPECT_FALSE(map_pair((*rows)[2], MarketType::Spot).has_value()) << "closed";
}

TEST(BitunixCatalogue, SpotDepthIsPolledAtTheTickPrecision) {
    const auto body = fixture("spot_rest_depth.json");
    LevelBuffer storage;
    const auto wire = parse_spot_depth(body, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_FALSE(wire->bids.empty());
    ASSERT_FALSE(wire->asks.empty());
    EXPECT_EQ(wire->asks.front().price, "84854.01");
    EXPECT_EQ(wire->asks.front().qty, "0.1027");
    EXPECT_EQ(precision_text(e9("0.01")), "0.01");
    EXPECT_EQ(precision_text(e9("1")), "1");
    EXPECT_EQ(precision_text(e9("0.00001")), "0.00001");
}

TEST(BitunixRest, OverloadIsAnHttp200WithCode100009) {
    const auto body = fixture("rest_rate_limited.json");
    const auto status = parse_status(body);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->code, k_rate_limited);
    EXPECT_EQ(status->msg, "System error");
}

TEST(BitunixMap, PercentRatesBecomeFractions) {
    EXPECT_EQ(percent_to_fraction("0.3"), e9("0.003"));
    EXPECT_EQ(percent_to_fraction("-0.001352"), e9("-0.00001352"));
    EXPECT_EQ(step_of(0), e9("1"));
    EXPECT_EQ(step_of(4), e9("0.0001"));
    EXPECT_FALSE(step_of(10).has_value());
}

TEST(BitunixFunding, TheBatchRateIsPercentAndCarriesItsOwnInterval) {
    const auto body = fixture("rest_funding_batch.json");
    const auto storage = std::make_unique<FundingBuffer>();
    const auto rows = parse_funding(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);
    const auto row = std::ranges::find(*rows, std::string_view{"BTCUSDT"}, &WireFunding::symbol);
    ASSERT_NE(row, rows->end());
    const auto update = map_funding(*row, btc(), k_ingest);
    ASSERT_TRUE(update.has_value());
    EXPECT_EQ(update->rate, e9("-0.0000196"));
    EXPECT_EQ(update->interval_s, 8U * 3600U);
    EXPECT_EQ(update->next_funding_time.ns, 1'790'985'600'000'000'000ULL);
    EXPECT_EQ(update->mark_price.raw, 848275);
    EXPECT_EQ(update->accrual, FundingAccrual::Discrete);
}

TEST(BitunixTicker, ReadsBothVolumes) {
    const auto body = fixture("rest_tickers.json");
    const auto storage = std::make_unique<TickerBuffer>();
    const auto rows = parse_tickers(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 3U);
    const auto row = std::ranges::find(*rows, std::string_view{"BTCUSDT"}, &WireTicker::symbol);
    ASSERT_NE(row, rows->end());
    const auto stats = map_ticker_stats(*row, btc(), k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base, e9("39391.8538"));
    EXPECT_EQ(stats->volume_24h_quote.raw, 3'379'480'939'529'850LL);
}

TEST(BitunixStream, ClassifiesEveryFrame) {
    EXPECT_EQ(parse_head(fixture("ws_connect.json")).channel, Channel::Connect);
    EXPECT_EQ(parse_head(fixture("ws_pong.json")).channel, Channel::Pong);
    EXPECT_EQ(parse_head(fixture("ws_depth_book1.json")).channel, Channel::Book);
    EXPECT_EQ(parse_head(fixture("ws_depth_book15.json")).channel, Channel::Book);
    EXPECT_EQ(parse_head(fixture("ws_trade.json")).channel, Channel::Trade);
    EXPECT_EQ(parse_head(R"({"op":"subscribe","data":{"msg":"subscribe channel > 300","result":false}})").channel,
              Channel::Refused);
    EXPECT_EQ(parse_head("not json").channel, Channel::Unknown);
}

TEST(BitunixStream, TopOfBookCarriesTheExchangeStamp) {
    const auto body = fixture("ws_depth_book1.json");
    LevelBuffer storage;
    const auto wire = parse_book(body, storage);
    ASSERT_TRUE(wire.has_value());
    const auto bbo = map_bbo(*wire, btc(), k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 848122);
    EXPECT_EQ(bbo->ask.raw, 848123);
    EXPECT_EQ(bbo->bid_qty.raw, 24888);
    EXPECT_EQ(bbo->ask_qty.raw, 7516);
    EXPECT_EQ(bbo->ts_event.ns, 1'790'964'212'028'000'000ULL);
    EXPECT_EQ(bbo->ts_init, k_ingest);
}

TEST(BitunixStream, DepthIsAWholeLadder) {
    const auto body = fixture("ws_depth_book15.json");
    LevelBuffer storage;
    const auto wire = parse_book(body, storage);
    ASSERT_TRUE(wire.has_value());
    const auto book = map_book(*wire, btc(), k_ingest);
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->bid_count, 15U);
    EXPECT_EQ(book->ask_count, 15U);
    EXPECT_EQ(book->bids[14].price.raw, 848101);
    EXPECT_EQ(book->asks[14].price.raw, 848148);
    EXPECT_EQ(book->ts_event.ns, 1'790'964'212'032'000'000ULL);
}

TEST(BitunixStream, PrintsTakeTheEnvelopeMillisecond) {
    const auto body = fixture("ws_trade.json");
    PrintBuffer storage;
    const auto wire = parse_trades(body, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->prints.size(), 1U);
    const auto trade = map_trade(wire->prints[0], wire->ts_ms, btc(), k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->price.raw, 848123);
    EXPECT_EQ(trade->qty.raw, 16);
    EXPECT_EQ(trade->ts_event.ns, 1'790'964'212'024'000'000ULL);
}

TEST(BitunixCaps, ServesPerpAndSpotMarketDataAndPerpOrdersOverRest) {
    const auto& caps = exec::caps_of(VenueId::Bitunix);
    EXPECT_EQ(caps.venue, VenueId::Bitunix);
    EXPECT_TRUE(caps.markets.perp);
    EXPECT_TRUE(caps.markets.spot);
    EXPECT_FALSE(caps.fix_order_entry);
    EXPECT_FALSE(caps.ws_order_entry);
    EXPECT_TRUE(caps.amend_order);
    EXPECT_TRUE(caps.order_pull.perp);
    EXPECT_FALSE(caps.order_pull.spot);
}

void credentials(net::Signer& signer, std::string_view key, std::string_view secret) {
    signer.set_api_key(key);
    ASSERT_TRUE(signer.secret().assign(secret).has_value());
}

TEST(BitunixSigner, MatchesTheOfficialSdkVector) {
    net::Signer signer;
    credentials(signer, "11111111111111111", "22222222222222222");
    std::array<char, net::k_hex_bytes> digest{};
    EXPECT_EQ(net::sha256_hex("33333333333333333174710471126211111111111111111marginCoinUSDT", digest),
              "ea0f6d6bc37d102c41ccbd820f4cfbb10663f3675a75aea80a353e0e317fcdcc");
    std::array<char, net::k_hex_bytes> out{};
    EXPECT_EQ(sign_request(signer, "33333333333333333", "1747104711262", "marginCoinUSDT", "", out),
              "40e87256554e263e39201a3979469b7fbb640d1c7c41759abf5c6a18f937f922");
}

TEST(BitunixSigner, SignsTheBodyOfTheDocumentedExample) {
    net::Signer signer;
    credentials(signer, "yourApiKey", "yourSecretKey");
    std::array<char, net::k_hex_bytes> out{};
    EXPECT_EQ(sign_request(signer,
                           "123456",
                           "20241120123045",
                           "id1uid200",
                           R"({"uid":"2899","arr":[{"id":1,"name":"maple"},{"id":2,"name":"lily"}]})",
                           out),
              "00397cd1e52c7dce3258067324363b6361fabc9178a0912b330c138db8745655");
}

exec::InstrumentTable table_of_btc() {
    exec::InstrumentTable table;
    const auto instrument = map_pair(pair("BTCUSDT"), MarketType::Perp);
    EXPECT_TRUE(instrument.has_value() && table.add(*instrument).has_value());
    return table;
}

std::string_view header(const net::RestRequest& request, std::string_view name) {
    for (const auto& h : request.headers) {
        if (h.name == name) {
            return h.value;
        }
    }
    return {};
}

TEST(BitunixOrders, PlacesALimitWithItsClientIdAndSignsTheCompactBody) {
    net::Signer signer;
    credentials(signer, "key", "secret");
    const auto table = table_of_btc();
    RequestBuilder builder{signer};
    OrderCmd cmd{};
    cmd.instrument = base_of(*table.find("BTCUSDT")).id;
    cmd.side = Side::Sell;
    cmd.flags = OrderFlags::PostOnly;
    cmd.price = PriceTicks::from_raw(848123);
    cmd.qty = QtyLots::from_raw(16);
    const auto client_id = ExchangeClientOrderId::from("01JABCDEFGHJKMNPQRSTVWXYZ0");
    ASSERT_TRUE(client_id.has_value());
    cmd.exchange_client_order_id = *client_id;
    const auto request = builder.place(cmd, table, k_ingest);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->target, ep::k_place_order);
    EXPECT_EQ(request->body,
              R"({"symbol":"BTCUSDT","qty":"0.0016","price":"84812.3","effect":"POST_ONLY",)"
              R"("side":"SELL","orderType":"LIMIT","clientId":"01JABCDEFGHJKMNPQRSTVWXYZ0",)"
              R"("reduceOnly":false})");
    EXPECT_EQ(header(*request, "timestamp"), "1790964213000");
    EXPECT_EQ(header(*request, "nonce").size(), 32U);
    std::array<char, net::k_hex_bytes> expect{};
    EXPECT_EQ(header(*request, "sign"),
              sign_request(signer, header(*request, "nonce"), "1790964213000", "", request->body, expect));
}

TEST(BitunixOrders, CancelsAndModifiesByVenueIdWhenItHasOne) {
    net::Signer signer;
    credentials(signer, "key", "secret");
    const auto table = table_of_btc();
    RequestBuilder builder{signer};
    OrderCmd cmd{};
    cmd.instrument = base_of(*table.find("BTCUSDT")).id;
    const auto order_id = ExchangeOrderId::from("11111");
    ASSERT_TRUE(order_id.has_value());
    cmd.target_exchange_order_id = *order_id;
    cmd.price = PriceTicks::from_raw(600000);
    cmd.qty = QtyLots::from_raw(5000);
    const auto cancel = builder.cancel(cmd, table, k_ingest);
    ASSERT_TRUE(cancel.has_value());
    EXPECT_EQ(cancel->body, R"({"symbol":"BTCUSDT","orderList":[{"orderId":"11111"}]})");
    const auto modify = builder.modify(cmd, table, k_ingest);
    ASSERT_TRUE(modify.has_value());
    EXPECT_EQ(modify->target, ep::k_modify_order);
    EXPECT_EQ(modify->body, R"({"orderId":"11111","symbol":"BTCUSDT","qty":"0.5","price":"60000"})");
    const auto account = builder.account(k_ingest);
    ASSERT_TRUE(account.has_value());
    EXPECT_EQ(account->target, "/api/v1/futures/account?marginCoin=USDT");
}

TEST(BitunixOrders, NothingIsBuiltWithoutCredentials) {
    net::Signer signer;
    RequestBuilder builder{signer};
    const auto table = table_of_btc();
    OrderCmd cmd{};
    cmd.instrument = base_of(*table.find("BTCUSDT")).id;
    cmd.qty = QtyLots::from_raw(1);
    cmd.price = PriceTicks::from_raw(1);
    EXPECT_EQ(builder.place(cmd, table, k_ingest).error(), BuildError::NotSigned);
    EXPECT_EQ(builder.pending_orders(k_ingest).error(), BuildError::NotSigned);
}

class CaptureSink final : public exec::GatewaySink {
public:
    void on_bbo(const Bbo&) noexcept override {}

    void on_trade(const TradeTick&) noexcept override {}

    void on_funding(const FundingUpdate&) noexcept override {}

    void on_order_event(const OrderEvent& event) noexcept override { events.push_back(event); }

    void on_connectivity(const exec::ConnEvent&) noexcept override {}

    std::vector<OrderEvent> events;
};

TEST(BitunixVenue, ExecutionRefusesEveryCallWithoutCredentials) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const exec::VenueConfig config{.venue = VenueId::Bitunix, .market = MarketType::Perp};
    auto adapter = exec::make_venue(config, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_FALSE(adapter->execution->connected());
    OrderCmd cmd{};
    cmd.instrument = InstrumentId::make(VenueId::Bitunix, MarketType::Perp, SymbolId{0});
    cmd.client_order_id = ClientOrderId::make(1, 42);
    cmd.qty = QtyLots::from_raw(7);
    adapter->execution->submit(cmd);
    adapter->execution->cancel(cmd);
    adapter->execution->amend(cmd);
    ASSERT_EQ(sink.events.size(), 3U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);
    EXPECT_EQ(sink.events[0].leaves_qty.raw, 0);
    EXPECT_EQ(sink.events[1].type, OrderEventType::CancelRejected);
    EXPECT_EQ(sink.events[2].type, OrderEventType::ModifyRejected);
    const auto open = adapter->execution->open_order_reports(MarketType::Perp);
    ASSERT_FALSE(open.has_value());
    EXPECT_EQ(open.error().cls, exec::ErrorClass::AuthFailure);
    EXPECT_FALSE(adapter->execution->position_reports(MarketType::Perp).has_value());
    EXPECT_FALSE(adapter->execution->fill_reports(MarketType::Perp, UnixNanos{}).has_value());
    EXPECT_FALSE(adapter->execution->balance_reports(MarketType::Perp).has_value());
}

TEST(BitunixOrders, ClassifiesTheOverloadCodeAsRateLimited) {
    EXPECT_EQ(classify(k_rate_limited), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(10006), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(10007), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify(30042), exec::ErrorClass::Terminal);
}

TEST(BitunixOrders, ReadsTheDocumentedAnswers) {
    const auto table = table_of_btc();
    const auto place = fixture("doc_place_order.json");
    const auto ack = parse_ack(place);
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->order_id, "11111");

    const auto cancel_body = fixture("doc_cancel_orders.json");
    CancelBuffer cancels;
    const auto rows = parse_cancels(cancel_body, cancels);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 2U);
    EXPECT_TRUE((*rows)[0].ok);
    EXPECT_FALSE((*rows)[1].ok);
    EXPECT_EQ((*rows)[1].error_code, 10013);

    const auto orders_body = fixture("doc_pending_orders.json");
    const auto orders = std::make_unique<OrderBuffer>();
    const auto open = parse_orders(orders_body, *orders);
    ASSERT_TRUE(open.has_value());
    ASSERT_EQ(open->size(), 1U);
    EXPECT_FALSE(map_order((*open)[0], table).has_value()) << "the documented row names no side";
    WireOrder sided_order = (*open)[0];
    sided_order.side = "BUY";
    const auto report = map_order(sided_order, table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(report->leaves.raw, 5000);

    const auto fills_body = fixture("doc_history_trades.json");
    const auto fills = std::make_unique<FillBuffer>();
    const auto trades = parse_fills(fills_body, *fills);
    ASSERT_TRUE(trades.has_value());
    ASSERT_EQ(trades->size(), 1U);
    EXPECT_EQ((*trades)[0].role, "TAKER");
    EXPECT_EQ((*trades)[0].ctime_ms, 1'597'026'383'085ULL);
    WireFill sided = (*trades)[0];
    sided.side = "BUY";
    const auto fill = map_fill(sided, table);
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->liquidity, Liquidity::Taker);
    EXPECT_EQ(fill->qty.raw, 10000);
    EXPECT_EQ(fill->price.raw, 600000);
    EXPECT_EQ(fill->fee, e9("0.01"));

    const auto positions_body = fixture("doc_pending_positions.json");
    const auto positions = std::make_unique<PositionBuffer>();
    const auto held = parse_positions(positions_body, *positions);
    ASSERT_TRUE(held.has_value());
    ASSERT_EQ(held->size(), 1U);
    EXPECT_EQ(map_position((*held)[0], table)->net_qty.raw, 5000);
    WirePosition short_one = (*held)[0];
    short_one.side = "SHORT";
    EXPECT_EQ(map_position(short_one, table)->net_qty.raw, -5000);

    const auto account_body = fixture("doc_account.json");
    BalanceBuffer balances;
    const auto coins = parse_balances(account_body, balances);
    ASSERT_TRUE(coins.has_value());
    const auto balance = map_balance((*coins)[0], k_ingest);
    ASSERT_TRUE(balance.has_value());
    EXPECT_EQ(balance->available, e9("1000"));
    EXPECT_EQ(balance->total, e9("1010"));
    EXPECT_EQ(balance->unrealized, e9("2"));
}

} // namespace
} // namespace raikiri::venues::bitunix
