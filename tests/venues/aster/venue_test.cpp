// aster parser tests against captured bytes; see fixtures/README.md

#include <raikiri/domain/asset_class.hpp>
#include <raikiri/venues/aster/book.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/venues/aster/catalogue.hpp>
#include <raikiri/venues/aster/endpoints.hpp>
#include <raikiri/venues/aster/envelope.hpp>
#include <raikiri/venues/aster/errors.hpp>
#include <raikiri/venues/aster/map.hpp>
#include <raikiri/venues/aster/market_data.hpp>
#include <raikiri/venues/aster/rest.hpp>
#include <raikiri/venues/aster/ws.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace raikiri::venues::aster {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// every parser takes the payload, not the {"stream":..,"data":..} wrapper
std::string data_of(std::string_view name) {
    const auto frame = fixture(name);
    const auto inner = unwrap(frame);
    EXPECT_TRUE(inner.has_value());
    return std::string{*inner};
}

constexpr UnixNanos k_ingest{1'700'000'000'000'000'000ULL};

exec::InstrumentTable table_with_btc() {
    exec::InstrumentTable table;
    WireInstrument wire{};
    wire.symbol = "BTCUSDT";
    wire.base_asset = "BTC";
    wire.quote_asset = "USDT";
    wire.contract_type = "PERPETUAL";
    wire.status = "TRADING";
    wire.tick_size = "0.10";
    wire.step_size = "0.001";
    wire.min_notional = "5";
    wire.funding_interval_s = 28'800;
    const auto instrument = map_instrument(wire, MarketType::Perp);
    EXPECT_TRUE(instrument.has_value());
    EXPECT_TRUE(table.add(*instrument).has_value());
    return table;
}

TEST(AsterWire, ExchangeInfoParsesTheThreeShapesTheVenueServes) {
    InstrumentBuffer storage;
    const auto body = fixture("rest_exchange_info.json");
    const auto rows = parse_exchange_info(body, storage);
    ASSERT_TRUE(rows.has_value());
    // the fixture keeps one plain perp, one non-usdt quote and one not trading
    ASSERT_EQ(rows->size(), 3U);

    bool saw_btc = false;
    for (const auto& row : *rows) {
        if (row.symbol == "BTCUSDT") {
            saw_btc = true;
            EXPECT_EQ(row.contract_type, "PERPETUAL");
            EXPECT_EQ(row.quote_asset, "USDT");
        }
    }
    EXPECT_TRUE(saw_btc);
}

TEST(AsterMap, ANonUsdtQuoteSurvivesTheMapper) {
    // rows quoted in USD1 and "U" must map too
    WireInstrument wire{};
    wire.symbol = "BTCUSD1";
    wire.base_asset = "BTC";
    wire.quote_asset = "USD1";
    wire.contract_type = "PERPETUAL";
    wire.status = "TRADING";
    wire.tick_size = "0.10";
    wire.step_size = "0.001";
    wire.funding_interval_s = 28'800;
    const auto instrument = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
}

TEST(AsterMap, ARowThatIsNotTradingIsRefused) {
    WireInstrument wire{};
    wire.symbol = "PENDINGUSDT";
    wire.base_asset = "PENDING";
    wire.quote_asset = "USDT";
    wire.contract_type = "PERPETUAL";
    wire.status = "PENDING_TRADING";
    wire.tick_size = "0.10";
    wire.step_size = "0.001";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
}

TEST(AsterWire, EventDispatchResolvesKindAndSymbol) {
    for (const auto& [file, kind] : {std::pair{"ws_book_ticker.json", EventKind::BookTicker},
                                     std::pair{"ws_agg_trade.json", EventKind::Trade},
                                     std::pair{"ws_mark_price.json", EventKind::MarkPrice},
                                     std::pair{"ws_depth_update.json", EventKind::DepthUpdate}}) {
        const auto frame = fixture(file);
        const auto event = parse_event(frame);
        ASSERT_TRUE(event.has_value()) << file;
        EXPECT_EQ(event->kind, kind) << file;
        EXPECT_EQ(event->symbol, "BTCUSDT") << file;
    }
}

TEST(AsterMap, BookTickerBecomesBbo) {
    const auto table = table_with_btc();
    const auto payload = data_of("ws_book_ticker.json");
    const auto wire = parse_book_ticker(payload);
    ASSERT_TRUE(wire.has_value());
    const auto bbo = map_bbo(*wire, table, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_GT(bbo->bid.raw, 0);
    EXPECT_GT(bbo->ask.raw, bbo->bid.raw);
    EXPECT_NE(bbo->ts_event.ns, 0ULL);
}

TEST(AsterMap, MarkPriceCarriesTheFundingRate) {
    const auto table = table_with_btc();
    const auto payload = data_of("ws_mark_price.json");
    const auto wire = parse_mark_price(payload);
    ASSERT_TRUE(wire.has_value());
    const auto funding = map_funding(*wire, table, k_ingest);
    ASSERT_TRUE(funding.has_value());
    // the mapper may refuse a contract quoting no rate; this fixture quotes one
    EXPECT_NE(funding->next_funding_time.ns, 0ULL);
    EXPECT_EQ(funding->mark_price.raw, 803'296);  // 80329.60794203 truncated onto the 0.10 tick
    EXPECT_EQ(funding->index_price.raw, 803'590); // 80359.00804348, likewise
    EXPECT_TRUE(funding->predicted_rate.is_undef());
    EXPECT_EQ(funding->interval_s, 28'800U);
    EXPECT_EQ(funding->ts_event.ns, 1'789'901'124'000'000'000ULL);
}

TEST(AsterBook, ConsecutiveDepthFramesChainByPrevUpdateId) {
    // frame N's `u` is frame N+1's `pu`
    const auto first = data_of("ws_depth_update.json");
    const auto second = data_of("ws_depth_update_next.json");
    LevelBuffer buf_a;
    LevelBuffer buf_b;
    const auto a = parse_depth_update(first, buf_a);
    const auto b = parse_depth_update(second, buf_b);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(a->last_id, b->prev_id);
    EXPECT_LE(a->first_id, a->last_id);
}

TEST(AsterEndpoints, TestnetHasItsOwnHostsAndChain) {
    EXPECT_EQ(ep::rest_host(true), ep::k_rest_test);
    EXPECT_EQ(ep::stream_host(true), ep::k_stream_test);
    EXPECT_EQ(ep::rest_host(false), ep::k_rest_live);
    EXPECT_EQ(ep::chain_id(false), 1666U);
    EXPECT_EQ(ep::chain_id(true), 714U);
}

TEST(AsterMap, TheSubTypeTellsAStockFromACoin) {
    InstrumentBuffer storage;
    const auto body = fixture("rest_exchange_info_tradfi.json");
    const auto rows = parse_exchange_info(body, storage);
    ASSERT_TRUE(rows.has_value());
    std::map<std::string, std::string, std::less<>> keys;
    for (const auto& row : *rows) {
        auto instrument = map_instrument(row, MarketType::Perp);
        ASSERT_TRUE(instrument.has_value()) << row.symbol;
        classify(VenueId::Aster, *instrument);
        keys.emplace(std::string{row.symbol}, asset_key(base_of(*instrument)));
    }
    EXPECT_EQ(keys["BTCUSDT"], "crypto:BTC");
    EXPECT_EQ(keys["AAPLUSDT"], "equity:AAPL");
    EXPECT_EQ(keys["IWMUSDT"], "equity:IWM");
    EXPECT_EQ(keys["XAGUSDT"], "commodity:XAG");
}

exec::InstrumentTable spot_table() {
    static thread_local InstrumentBuffer storage;
    static const auto body = fixture("spot_rest_exchange_info.json");
    const auto rows = parse_exchange_info(body, storage);
    EXPECT_TRUE(rows.has_value());
    exec::InstrumentTable table;
    for (const auto& row : *rows) {
        if (auto instrument = map_instrument(row, MarketType::Spot); instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value()) << row.symbol;
        }
    }
    return table;
}

TEST(AsterSpot, TheSpotCatalogueLoadsAsSpotAndNeverAsPerp) {
    static thread_local InstrumentBuffer storage;
    const auto body = fixture("spot_rest_exchange_info.json");
    const auto rows = parse_exchange_info(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);
    for (const auto& row : *rows) {
        EXPECT_FALSE(map_instrument(row, MarketType::Perp).has_value()) << row.symbol;
    }
    const auto table = spot_table();
    EXPECT_EQ(table.size(), 4U);
    const auto* btc = table.find("BTCUSDT");
    ASSERT_NE(btc, nullptr);
    ASSERT_TRUE(std::holds_alternative<SpotInstrument>(*btc));
    const auto& base = base_of(*btc);
    EXPECT_EQ(base.id.market(), MarketType::Spot);
    EXPECT_EQ(base.local_symbol.view(), "BTC/USDT");
    EXPECT_EQ(base.tick_size.raw, 10'000'000);        // 0.01
    EXPECT_EQ(base.lot_size.raw, 10'000);             // 0.00001
    EXPECT_EQ(base.min_notional.raw, 10'000'000'000); // spot spells it minNotional
    EXPECT_EQ(base.asset_class, AssetClass::Crypto);
    EXPECT_EQ(base_of(*table.find("WLFIUSD1")).local_symbol.view(), "WLFI/USD1");
}

TEST(AsterSpot, ThePerpCatalogueStillReadsItsOwnMinimum) {
    InstrumentBuffer storage;
    const auto body = fixture("rest_exchange_info.json");
    const auto rows = parse_exchange_info(body, storage);
    ASSERT_TRUE(rows.has_value());
    const auto perp = std::ranges::find_if(*rows, [](const WireInstrument& row) { return !row.min_notional.empty(); });
    EXPECT_NE(perp, rows->end());
}

TEST(AsterSpot, QuotesPrintsAndBooksShareTheFuturesShapes) {
    const auto table = spot_table();
    const auto quote_payload = data_of("spot_ws_book_ticker.json");
    const auto quote = parse_book_ticker(quote_payload);
    ASSERT_TRUE(quote.has_value());
    const auto bbo = map_bbo(*quote, table, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 8'659'152); // 86591.52 / 0.01
    EXPECT_EQ(bbo->ask.raw, 8'659'239);
    EXPECT_NE(bbo->ts_event.ns, 0ULL);

    const auto print_payload = data_of("spot_ws_trade.json");
    const auto print = parse_trade(print_payload);
    ASSERT_TRUE(print.has_value());
    const auto trade = map_trade(*print, table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->price.raw, 2'800); // 0.2800 / 0.0001
    EXPECT_EQ(trade->qty.raw, 422);     // 42.20 base / 0.10
    EXPECT_EQ(trade->aggressor, Side::Sell);

    LevelBuffer buf_a;
    LevelBuffer buf_b;
    const auto first = data_of("spot_ws_depth_update.json");
    const auto second = data_of("spot_ws_depth_update_next.json");
    const auto a = parse_depth_update(first, buf_a);
    const auto b = parse_depth_update(second, buf_b);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(a->last_id, b->prev_id);
    LevelBuffer seed;
    const auto seed_body = fixture("spot_rest_depth_snapshot.json");
    EXPECT_TRUE(parse_depth_snapshot(seed_body, seed).has_value());
}

TEST(AsterSpot, TheDailyTableCarriesBothVolumeColumns) {
    const auto table = spot_table();
    static thread_local Ticker24hBuffer storage;
    const auto body = fixture("spot_rest_ticker_24h.json");
    const auto rows = parse_ticker_24h(body, storage);
    ASSERT_TRUE(rows.has_value());
    const auto btc = std::ranges::find_if(*rows, [](const WireTicker24h& row) { return row.symbol == "BTCUSDT"; });
    ASSERT_NE(btc, rows->end());
    const auto stats = map_ticker_stats(*btc, table, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->instrument.market(), MarketType::Spot);
    EXPECT_EQ(stats->volume_24h_base.raw, 4'735'920'000);    // 4.73592
    EXPECT_EQ(stats->volume_24h_quote.raw, 406'446'123'515); // 406446.123515
    EXPECT_EQ(stats->ts_event.ns, 1'791'299'537'187'000'000ULL);
}

TEST(AsterSpot, SpotAddressesItsOwnHostsAndPaths) {
    EXPECT_EQ(rest_host(false, MarketType::Spot), ep::k_spot_rest_live);
    EXPECT_EQ(stream_host(false, MarketType::Spot), ep::k_spot_stream_live);
    EXPECT_EQ(rest_host(false), ep::k_rest_live);
    const net::Signer signer;
    RequestBuilder spot{signer, MarketType::Spot};
    EXPECT_EQ(spot.instruments().target, ep::k_spot_exchange_info);
    EXPECT_EQ(spot.ticker_24h().target, ep::k_spot_ticker_24h);
    EXPECT_EQ(spot.server_time().target, ep::k_spot_server_time);
    EXPECT_TRUE(spot.depth_snapshot("BTCUSDT", 1000).target.starts_with(ep::k_spot_depth));
    RequestBuilder perp{signer};
    EXPECT_EQ(perp.instruments().target, ep::k_exchange_info);
    const auto time_body = fixture("spot_rest_time.json");
    const auto now = parse_server_time(time_body);
    ASSERT_TRUE(now.has_value());
    EXPECT_GT(*now, 1'791'000'000'000ULL);
}

// the docs publish this agent key and its address; the digest and signature were computed with eth-account
constexpr std::string_view k_demo_agent_key = "0x4fd0a42218f3eae43a6ce26d22544e986139a01e5b34a62db53757ffca81bae1";
constexpr std::string_view k_demo_user = "0x63DD5aCC6b1aa0f563956C0e534DD30B6dcF7C4e";
constexpr std::string_view k_demo_msg =
    "symbol=ASTERUSDT&type=LIMIT&side=BUY&timeInForce=GTC&quantity=20&price=0.5&nonce=1748310859508867"
    "&user=0x63DD5aCC6b1aa0f563956C0e534DD30B6dcF7C4e&signer=0x21cF8Ae13Bb72632562c6Fff438652Ba1a151bb0";

std::string hex_of(const net::evm::Hash& hash) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string out{"0x"};
    for (const auto byte : hash) {
        out.push_back(digits[byte >> 4U]);
        out.push_back(digits[byte & 0x0FU]);
    }
    return out;
}

ExchangeClientOrderId coid() {
    return *ExchangeClientOrderId::from("01HZY3M7K8Q9R0S1T2V3W4X5Y6");
}

TEST(AsterSigner, MatchesTheDocumentedAgentAndTheEip712Vectors) {
    net::evm::Wallet agent;
    ASSERT_TRUE(agent.load(k_demo_agent_key));
    EXPECT_EQ(agent.address(), "0x21cf8ae13bb72632562c6fff438652ba1a151bb0");
    EXPECT_EQ(hex_of(message_digest(k_demo_msg, 1666)),
              "0x214122039b686e20a15baab91d368d354cf8c88067cb8b0e96e9018d8b08a625");
    EXPECT_EQ(hex_of(message_digest(k_demo_msg, 714)),
              "0xd0a3cddc3992449300d5b6fa97e441d66d0f950a032b0707455996c29d8477c5");
    std::array<char, net::evm::k_rsv_hex_bytes> out{};
    EXPECT_EQ(sign_message(agent, k_demo_msg, 1666, out),
              "0x0a56c5923ebf3524475c5f631940ec4c0e41dbd300ad28198d1915d8f3ca49ce"
              "26fd1aeb8b0c3079133595da0de38322ae7e860e0c76d032c4a491d27c1430b01c");
    EXPECT_EQ(sign_message(agent, k_demo_msg, 714, out),
              "0xf91e000376f360c1661cf0fbc79a816e915dce485c616bd49c5e35c6cbc5ba50"
              "42ccf75a2bffd282676dbf698087a73982bfd1491a993eeedc276f6827c94a8c1c");
}

TEST(AsterRequests, SignsTheQueryItSendsWithTheAuthTrioAndRefusesWithoutAWallet) {
    const auto table = table_with_btc();
    const auto& base = base_of(*table.find("BTCUSDT"));
    net::Signer signer;
    net::evm::Wallet agent;
    RequestBuilder unkeyed{signer};
    OrderCmd cmd{};
    cmd.instrument = base.id;
    cmd.side = Side::Sell;
    cmd.flags = OrderFlags::PostOnly;
    cmd.qty = QtyLots::from_raw(5);
    cmd.price = PriceTicks::from_raw(848001);
    cmd.exchange_client_order_id = coid();
    const UnixNanos now{1'748'310'859'508'867'123ULL};
    ASSERT_EQ(unkeyed.create_order(cmd, base, now).error(), BuildError::NoCredentials);

    signer.set_api_key(k_demo_user);
    ASSERT_TRUE(agent.load(k_demo_agent_key));
    RequestBuilder builder{signer};
    builder.set_wallet(agent, false);
    const auto request = builder.create_order(cmd, base, now);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(builder.last_sign_message(),
              "symbol=BTCUSDT&side=SELL&type=LIMIT&timeInForce=GTX&quantity=0.005&price=84800.1"
              "&newClientOrderId=01HZY3M7K8Q9R0S1T2V3W4X5Y6&nonce=1748310859508867"
              "&user=0x63DD5aCC6b1aa0f563956C0e534DD30B6dcF7C4e&signer=0x21cf8ae13bb72632562c6fff438652ba1a151bb0");
    std::array<char, net::evm::k_rsv_hex_bytes> out{};
    EXPECT_EQ(request->target,
              std::string{ep::k_order} + "?" + std::string{builder.last_sign_message()} +
                  "&signature=" + std::string{sign_message(agent, builder.last_sign_message(), 1666, out)});
    EXPECT_EQ(request->method, net::HttpMethod::Post);
    EXPECT_TRUE(request->headers.empty());

    // a second request in the same microsecond still gets a fresh nonce
    const auto cancel = builder.cancel_order(cmd, base, now);
    ASSERT_TRUE(cancel.has_value());
    EXPECT_EQ(cancel->method, net::HttpMethod::Delete);
    EXPECT_TRUE(builder.last_sign_message().starts_with(
        "symbol=BTCUSDT&origClientOrderId=01HZY3M7K8Q9R0S1T2V3W4X5Y6&nonce=1748310859508868&"));

    cmd.flags = OrderFlags::None;
    cmd.tif = TimeInForce::Ioc;
    ASSERT_TRUE(builder.create_order(cmd, base, now).has_value());
    EXPECT_NE(builder.last_sign_message().find("&timeInForce=IOC&"), std::string_view::npos);
    cmd.tif = TimeInForce::Fok;
    EXPECT_EQ(builder.create_order(cmd, base, now).error(), BuildError::UnsupportedOrderType);

    const auto key = builder.listen_key_create(now);
    ASSERT_TRUE(key.has_value());
    EXPECT_TRUE(builder.last_sign_message().starts_with("nonce="));
    EXPECT_TRUE(key->target.starts_with(std::string{ep::k_listen_key} + "?nonce="));

    builder.set_wallet(agent, true);
    ASSERT_TRUE(builder.open_orders(now).has_value());
    const auto testnet_sig = std::string{sign_message(agent, builder.last_sign_message(), 714, out)};
    EXPECT_TRUE(builder.open_orders(now).has_value());
    EXPECT_NE(testnet_sig, std::string{sign_message(agent, builder.last_sign_message(), 714, out)});
}

TEST(AsterErrors, ASignatureRefusalHaltsAndADepositGateIsNotRetried) {
    const auto bad_sig_body = fixture("rest_error_signature.json");
    const auto bad_sig = parse_rest_error(bad_sig_body);
    ASSERT_TRUE(bad_sig.has_value());
    EXPECT_EQ(bad_sig->code, k_unknown);
    EXPECT_EQ(classify(bad_sig->code, bad_sig->msg), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify(k_unknown, "An unknown error occured while processing the request."),
              exec::ErrorClass::Retryable);
    const auto no_nonce_body = fixture("rest_error_no_nonce.json");
    const auto no_nonce = parse_rest_error(no_nonce_body);
    ASSERT_TRUE(no_nonce.has_value());
    EXPECT_EQ(classify(no_nonce->code, no_nonce->msg), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_needs_deposit), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify(k_nonce_expired), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_too_many_orders), exec::ErrorClass::RateLimited);
    EXPECT_TRUE(is_benign_cancel_failure(k_no_such_order));
}

TEST(AsterUserStream, OrderTradeUpdatesBecomeLifecycleAndFills) {
    const auto table = table_with_btc();
    const auto frame = fixture("user_order_trade_update.json");
    const auto event = parse_event(frame);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::OrderUpdate);
    const auto accepted = parse_order_update(frame);
    ASSERT_TRUE(accepted.has_value());
    const auto mapped = map_order_event(*accepted, table, k_ingest);
    ASSERT_TRUE(mapped.has_value());
    EXPECT_EQ(mapped->type, OrderEventType::Accepted);
    EXPECT_EQ(mapped->side, Side::Sell);
    EXPECT_EQ(mapped->qty.raw, 1);
    EXPECT_EQ(mapped->exchange_order_id.view(), "8886774");

    const std::string fill{
        R"({"e":"ORDER_TRADE_UPDATE","E":1791700000001,"T":1791700000000,"o":{"s":"BTCUSDT",)"
        R"("c":"01HZY3M7K8Q9R0S1T2V3W4X5Y6","S":"BUY","o":"LIMIT","f":"GTX","q":"0.005","p":"84800.1","ap":"84800.1",)"
        R"("sp":"0","x":"TRADE","X":"PARTIALLY_FILLED","i":8886775,"l":"0.002","z":"0.002","L":"84800.1","N":"USDT",)"
        R"("n":"0.0339","T":1791700000000,"t":4242,"b":"0","a":"0","m":true,"R":false,"wt":"CONTRACT_PRICE",)"
        R"("ot":"LIMIT","ps":"BOTH","cp":false,"rp":"0"}})"};
    const auto trade = parse_order_update(fill);
    ASSERT_TRUE(trade.has_value());
    const auto filled = map_order_event(*trade, table, k_ingest);
    ASSERT_TRUE(filled.has_value());
    EXPECT_EQ(filled->type, OrderEventType::Filled);
    EXPECT_TRUE(has_flag(filled->flags, OrderFlags::PostOnly));
    EXPECT_EQ(filled->qty.raw, 2);
    EXPECT_EQ(filled->leaves_qty.raw, 3);
    EXPECT_EQ(filled->price.raw, 848001);
    EXPECT_EQ(filled->liquidity, Liquidity::Maker);
    EXPECT_EQ(filled->exec_id.view(), "4242");
    EXPECT_EQ(filled->exchange_client_order_id, coid());

    EXPECT_EQ(parse_event(fixture("user_listen_key_expired.json"))->kind, EventKind::ListenKeyExpired);
    EXPECT_EQ(parse_listen_key(fixture("rest_listen_key.json")).value_or(""),
              "pqia91ma19a5s61cv6a81va65sdf19v8a65a1a5s61cv6a81va65sdf19v8a65a1");
}

TEST(AsterReconciliation, ReadsTheV3OrdersFillsPositionsBalancesAndIncome) {
    const auto table = table_with_btc();
    // the doc's client id "abc" is not one this oms mints
    auto order_body = fixture("rest_query_order.json");
    order_body.replace(order_body.find("\"abc\""), 5, "\"01HZY3M7K8Q9R0S1T2V3W4X5Y6\"");
    const auto order = parse_order(order_body);
    ASSERT_TRUE(order.has_value());
    const auto report = map_open_order(*order, table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->qty.raw, 400);
    EXPECT_EQ(report->leaves.raw, 400);
    EXPECT_EQ(report->side, Side::Buy);

    const auto orders_storage = std::make_unique<OrderBuffer>();
    const auto open_body = fixture("rest_open_orders.json");
    const auto open = parse_orders(open_body, *orders_storage);
    ASSERT_TRUE(open.has_value());
    ASSERT_EQ(open->size(), 1U);

    const auto trades_storage = std::make_unique<TradeBuffer>();
    // the doc's 7819.01 is off this table's 0.10 tick
    auto trades_body = fixture("rest_user_trades.json");
    trades_body.replace(trades_body.find("7819.01"), 7, "7819.10");
    const auto trades = parse_user_trades(trades_body, *trades_storage);
    ASSERT_TRUE(trades.has_value());
    const auto fill = map_user_trade(trades->front(), table);
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->qty.raw, 2);
    EXPECT_EQ(fill->price.raw, 78191);
    EXPECT_EQ(fill->side, Side::Sell);
    EXPECT_EQ(fill->exec_id.view(), "698759");

    std::array<WirePositionRisk, 8> positions{};
    const auto risk_body = fixture("rest_position_risk.json");
    const auto risk = parse_position_risk(risk_body, positions);
    ASSERT_TRUE(risk.has_value());
    const auto position = map_position(risk->front(), table);
    ASSERT_TRUE(position.has_value());
    EXPECT_EQ(position->net_qty.raw, 0);

    const auto balance_storage = std::make_unique<BalanceBuffer>();
    const auto account_body = fixture("rest_account.json");
    const auto account = parse_account(account_body, *balance_storage);
    ASSERT_TRUE(account.has_value());
    ASSERT_FALSE(account->assets.empty());
    const auto usdt = map_balance(account->assets.front(), account->multi_assets_margin);
    ASSERT_TRUE(usdt.has_value());
    EXPECT_EQ(usdt->total, parse_decimal_e9("23.72469206").value());

    const auto income_storage = std::make_unique<IncomeBuffer>();
    const auto income_body = fixture("rest_income.json");
    const auto income = parse_income(income_body, *income_storage);
    ASSERT_TRUE(income.has_value());
    ASSERT_EQ(income->size(), 2U);
    EXPECT_EQ(income->back().income_type, "COMMISSION");
    EXPECT_EQ(income->back().trade_id, 2059192U);
}

TEST(AsterCaps, StatesRestOrderEntryOnPerpsAndAListenKey) {
    const auto& caps = exec::caps_of(VenueId::Aster);
    EXPECT_TRUE(caps.markets.perp);
    EXPECT_FALSE(caps.ws_order_entry);
    EXPECT_TRUE(caps.cancel_by_client_order_id);
    EXPECT_TRUE(caps.amend_order);
    EXPECT_TRUE(caps.native_post_only);
    EXPECT_TRUE(caps.order_pull.perp);
    EXPECT_FALSE(caps.order_pull.spot);
    EXPECT_EQ(caps.user_stream, exec::UserStreamModel::ListenKey);
    EXPECT_EQ(caps.max_client_order_id_len, 36);
}

} // namespace
} // namespace raikiri::venues::aster
