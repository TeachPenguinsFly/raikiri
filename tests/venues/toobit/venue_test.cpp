#include <raikiri/exec/caps.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/toobit/endpoints.hpp>
#include <raikiri/venues/toobit/map.hpp>
#include <raikiri/venues/toobit/rest.hpp>
#include <raikiri/venues/toobit/wire.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>

namespace raikiri::venues::toobit {
namespace {

const std::string& fixture(const std::string& name) {
    static std::map<std::string, std::string> loaded;
    if (const auto it = loaded.find(name); it != loaded.end()) {
        return it->second;
    }
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return loaded.emplace(name, buf.str()).first->second;
}

constexpr UnixNanos k_ingest{1'790'964'250'000'000'000ULL};

FixedE9 e9(std::string_view text) {
    return parse_decimal_e9(text).value_or(FixedE9{});
}

const WireMarket& row(std::string_view symbol) {
    static const auto body = fixture("rest_exchange_info.json");
    static const auto storage = std::make_unique<MarketBuffer>();
    static const auto rows = parse_exchange_info(body, *storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& one : *rows) {
        if (one.symbol == symbol) {
            return one;
        }
    }
    ADD_FAILURE() << "no market " << symbol;
    static const WireMarket empty{};
    return empty;
}

const WireFunding* funding_row(std::string_view symbol) {
    static const auto body = fixture("rest_funding_rate.json");
    static const auto storage = std::make_unique<FundingBuffer>();
    static const auto rows = parse_funding_rates(body, *storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& one : *rows) {
        if (one.symbol == symbol) {
            return &one;
        }
    }
    return nullptr;
}

InstrumentBase base_for(std::string_view symbol, MarketType market = MarketType::Perp) {
    const auto instrument = map_instrument(row(symbol), market, nullptr);
    EXPECT_TRUE(instrument.has_value()) << symbol;
    auto base = base_of(*instrument);
    base.id = InstrumentId::make(VenueId::Toobit, market, SymbolId{3});
    return base;
}

exec::InstrumentTable& table() {
    static exec::InstrumentTable loaded = [] {
        exec::InstrumentTable out;
        for (const auto* symbol : {"BTC-SWAP-USDT", "ETH-SWAP-USDT"}) {
            EXPECT_TRUE(out.add(*map_instrument(row(symbol), MarketType::Perp, nullptr)).has_value());
        }
        return out;
    }();
    return loaded;
}

std::string delta(std::string_view symbol, std::string_view t, std::string_view bids) {
    return std::string{R"({"symbol":")"} + std::string{symbol} + R"(","data":[{"t":)" + std::string{t} + R"(,"b":)" +
           std::string{bids} + R"(,"a":[]}],"f":false})";
}

ExchangeClientOrderId coid() {
    return *ExchangeClientOrderId::from("01HZY3M7K8Q9R0S1T2V3W4X5Y6");
}

TEST(ToobitCatalogue, KeepsTheUsdtPerpsAndTheSpotPairsTheApiTrades) {
    const auto body = fixture("rest_exchange_info.json");
    const auto storage = std::make_unique<MarketBuffer>();
    const auto rows = parse_exchange_info(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 9U);

    EXPECT_TRUE(map_instrument(row("BTC-SWAP-USDT"), MarketType::Perp, nullptr).has_value());
    EXPECT_TRUE(map_instrument(row("XAUT-SWAP-USDT"), MarketType::Perp, nullptr).has_value());
    EXPECT_FALSE(map_instrument(row("BTC-SWAP-USDC"), MarketType::Perp, nullptr).has_value());
    EXPECT_FALSE(map_instrument(row("BTC-SWAP-USDT"), MarketType::Spot, nullptr).has_value());
    EXPECT_TRUE(map_instrument(row("BTCUSDT"), MarketType::Spot, nullptr).has_value());
    EXPECT_FALSE(map_instrument(row("BTCUSDT"), MarketType::Perp, nullptr).has_value());
    EXPECT_FALSE(map_instrument(row("ELGUSDT"), MarketType::Spot, nullptr).has_value());
    EXPECT_FALSE(map_instrument(row("GAIAUSDT"), MarketType::Spot, nullptr).has_value());
}

TEST(ToobitCatalogue, CountsPerpQuantityInContracts) {
    const auto btc = base_for("BTC-SWAP-USDT");
    EXPECT_EQ(btc.tick_size, e9("0.1"));
    EXPECT_EQ(btc.lot_size, e9("0.1"));
    EXPECT_EQ(btc.contract_value, e9("0.001"));
    EXPECT_EQ(btc.kind, ContractKind::Linear);
    const auto eth = base_for("ETH-SWAP-USDT");
    EXPECT_EQ(eth.lot_size, e9("0.1"));
    EXPECT_EQ(eth.contract_value, e9("0.01"));
    const auto spot = base_for("BTCUSDT", MarketType::Spot);
    EXPECT_EQ(spot.lot_size, e9("0.00001"));
    EXPECT_EQ(spot.min_notional, e9("5"));
}

TEST(ToobitFunding, TakesTheIntervalAndBoundsFromTheRateTable) {
    EXPECT_EQ(interval_of("8H"), 28800U);
    EXPECT_EQ(interval_of("4H"), 14400U);
    EXPECT_EQ(interval_of("1H"), 3600U);
    EXPECT_EQ(interval_of("H"), 0U);
    const auto* btc = funding_row("BTC-SWAP-USDT");
    ASSERT_NE(btc, nullptr);
    ASSERT_NE(funding_row("AXS-SWAP-USDT"), nullptr);
    EXPECT_EQ(interval_of(funding_row("AXS-SWAP-USDT")->period), 14400U);
    const auto instrument = map_instrument(row("BTC-SWAP-USDT"), MarketType::Perp, btc);
    ASSERT_TRUE(instrument.has_value());
    const auto& perp = std::get<PerpInstrument>(*instrument);
    EXPECT_EQ(perp.funding_interval_s, 28800U);
    EXPECT_EQ(perp.funding_cap, e9("0.0024"));
    EXPECT_EQ(perp.funding_floor, e9("-0.0024"));

    const auto base = base_for("BTC-SWAP-USDT");
    const auto update = map_funding(*btc, base, PriceTicks::from_raw(848075), PriceTicks::from_raw(848120), k_ingest);
    ASSERT_TRUE(update.has_value());
    EXPECT_EQ(update->rate, e9("0.00003629"));
    EXPECT_EQ(update->interval_s, 28800U);
    EXPECT_EQ(update->next_funding_time.ns, 1'790'985'600'000ULL * 1'000'000ULL);
    EXPECT_EQ(update->mark_price.raw, 848075);
    EXPECT_EQ(update->index_price.raw, 848120);
    EXPECT_TRUE(update->predicted_rate.is_undef());
    EXPECT_EQ(update->ts_event.ns, 0U);
    EXPECT_EQ(update->accrual, FundingAccrual::Discrete);
}

TEST(ToobitFunding, TheIndexIsKeyedByTheUnderlyingAndTruncatedOntoTheTick) {
    const auto body = fixture("rest_index.json");
    const auto storage = std::make_unique<IndexBuffer>();
    const auto rows = parse_index_prices(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);
    EXPECT_EQ(index_key("BTC-SWAP-USDT"), "BTCUSDT");
    EXPECT_EQ(rows->front().underlying, index_key("BTC-SWAP-USDT"));
    EXPECT_EQ(rows->front().price, "86508.92130435");
    const auto index = map_index_price(rows->front().price, base_for("BTC-SWAP-USDT"));
    ASSERT_TRUE(index.has_value());
    EXPECT_EQ(index->raw, 865089); // tick 0.1
    EXPECT_FALSE(parse_index_prices(R"({"edp":{}})", *storage).has_value());
}

TEST(ToobitTicker, ReadsTheQuoteTurnover) {
    const auto body = fixture("rest_contract_ticker.json");
    const auto storage = std::make_unique<TickerBuffer>();
    const auto rows = parse_tickers(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 2U);
    const auto stats = map_ticker(rows->front(), base_for("BTC-SWAP-USDT"), k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_quote, *parse_turnover_e6("7194468847.52397"));
    EXPECT_EQ(stats->ts_event.ns, 1'790'964'214'906ULL * 1'000'000ULL);
}

TEST(ToobitStream, ClassifiesEveryFrameKind) {
    EXPECT_EQ(parse_head(fixture("ws_bbo.json"))->channel, Channel::Bbo);
    EXPECT_EQ(parse_head(fixture("ws_depth_delta.json"))->channel, Channel::Depth);
    EXPECT_EQ(parse_head(fixture("ws_trade.json"))->channel, Channel::Trade);
    EXPECT_EQ(parse_head(fixture("ws_mark.json"))->channel, Channel::Mark);
    EXPECT_EQ(parse_head(fixture("ws_pong.json"))->channel, Channel::Pong);
    const auto error = parse_head(fixture("ws_error.json"));
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->channel, Channel::Error);
    EXPECT_EQ(error->code, "-100010");
    const auto ack = parse_head(fixture("ws_bbo_ack.json"));
    ASSERT_TRUE(ack.has_value());
    EXPECT_TRUE(ack->first);
    EXPECT_EQ(ack->symbol, "BTC-SWAP-USDT");
    EXPECT_EQ(parse_head(fixture("ws_bbo.json"))->send_ms, 1'790'964'249'629ULL);
}

TEST(ToobitStream, MapsTheTopOfBookOnTheEngineClock) {
    const auto ack = parse_bbo(fixture("ws_bbo_ack.json"));
    ASSERT_TRUE(ack.has_value());
    EXPECT_FALSE(ack->has_value());

    const auto wire = parse_bbo(fixture("ws_bbo.json"));
    ASSERT_TRUE(wire.has_value() && wire->has_value());
    const auto bbo = map_bbo(**wire, base_for("BTC-SWAP-USDT"), k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 848000);
    EXPECT_EQ(bbo->ask.raw, 848001);
    EXPECT_EQ(bbo->bid_qty.raw, 238241);
    EXPECT_EQ(bbo->ask_qty.raw, 271654);
    EXPECT_EQ(bbo->ts_event.ns, 1'790'964'249'625ULL * 1'000'000ULL);
    EXPECT_EQ(bbo->ts_init, k_ingest);

    const auto spot = parse_bbo(fixture("ws_bbo_spot.json"));
    ASSERT_TRUE(spot.has_value() && spot->has_value());
    const auto spot_bbo = map_bbo(**spot, base_for("ETHUSDT", MarketType::Spot), k_ingest);
    ASSERT_TRUE(spot_bbo.has_value());
    EXPECT_EQ(spot_bbo->bid.raw, 268576);
    EXPECT_EQ(spot_bbo->bid_qty.raw, 462112);
}

TEST(ToobitDepth, SeedsFromTheSubscribeReplyAndAppliesChanges) {
    const auto base = base_for("BTC-SWAP-USDT");
    LevelBuffer storage;
    DepthBook book;

    const auto early = parse_depth(fixture("ws_depth_delta.json"), storage);
    ASSERT_TRUE(early.has_value() && early->has_value());
    EXPECT_EQ(book.apply(**early, base), DepthBook::Step::Waiting);

    const auto seed = parse_depth(fixture("ws_depth_snapshot.json"), storage);
    ASSERT_TRUE(seed.has_value() && seed->has_value());
    EXPECT_TRUE((*seed)->first);
    EXPECT_EQ((*seed)->bids.size(), 241U);
    ASSERT_EQ(book.apply(**seed, base), DepthBook::Step::Applied);
    auto snapshot = book.snapshot(base.id, k_ingest);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->bids[0].price.raw, 848000);
    EXPECT_EQ(snapshot->bids[0].qty.raw, 244720);
    EXPECT_EQ(snapshot->asks[0].price.raw, 848001);

    const auto delta = parse_depth(fixture("ws_depth_delta.json"), storage);
    ASSERT_TRUE(delta.has_value() && delta->has_value());
    ASSERT_EQ(book.apply(**delta, base), DepthBook::Step::Applied);
    snapshot = book.snapshot(base.id, k_ingest);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->bids[0].qty.raw, 238241);
    EXPECT_EQ(snapshot->ts_event.ns, 1'790'964'249'625ULL * 1'000'000ULL);

    const auto next = parse_depth(fixture("ws_depth_delta_next.json"), storage);
    ASSERT_TRUE(next.has_value() && next->has_value());
    ASSERT_EQ(book.apply(**next, base), DepthBook::Step::Applied);

    const auto stale = parse_depth(fixture("ws_depth_delta.json"), storage);
    ASSERT_TRUE(stale.has_value() && stale->has_value());
    EXPECT_EQ(book.apply(**stale, base), DepthBook::Step::Broken);
    EXPECT_FALSE(book.seeded());
}

TEST(ToobitDepth, ARemovedLevelLeavesAnOffTickLevelIsSkippedAndACrossedTouchBreaks) {
    const auto base = base_for("BTC-SWAP-USDT");
    LevelBuffer storage;
    DepthBook book;
    const auto seed = parse_depth(fixture("ws_depth_snapshot.json"), storage);
    ASSERT_TRUE(seed.has_value() && seed->has_value());
    ASSERT_EQ(book.apply(**seed, base), DepthBook::Step::Applied);

    const auto removal = delta("BTC-SWAP-USDT", "1790964249400", R"([["84800","0"]])");
    const auto gone = parse_depth(removal, storage);
    ASSERT_TRUE(gone.has_value() && gone->has_value());
    ASSERT_EQ(book.apply(**gone, base), DepthBook::Step::Applied);
    EXPECT_EQ(book.snapshot(base.id, k_ingest)->bids[0].price.raw, 847999);

    const auto off_tick = delta("BTC-SWAP-USDT", "1790964249450", R"([["84799.95","1"]])");
    const auto skipped = parse_depth(off_tick, storage);
    ASSERT_TRUE(skipped.has_value() && skipped->has_value());
    ASSERT_EQ(book.apply(**skipped, base), DepthBook::Step::Applied);
    EXPECT_EQ(book.snapshot(base.id, k_ingest)->bids[0].price.raw, 847999);

    const auto crossed = delta("BTC-SWAP-USDT", "1790964249500", R"([["84900","1"]])");
    const auto cross = parse_depth(crossed, storage);
    ASSERT_TRUE(cross.has_value() && cross->has_value());
    EXPECT_EQ(book.apply(**cross, base), DepthBook::Step::Broken);
}

TEST(ToobitDepth, FloorsASpotSizeOffTheLotGrid) {
    const auto base = base_for("ETHUSDT", MarketType::Spot);
    LevelBuffer storage;
    DepthBook book;
    const auto seed = parse_depth(fixture("ws_depth_snapshot_spot.json"), storage);
    ASSERT_TRUE(seed.has_value() && seed->has_value());
    ASSERT_EQ(book.apply(**seed, base), DepthBook::Step::Applied);
    const auto odd = delta("ETHUSDT", "1790964249300", R"([["2685.76","1.23456789"]])");
    const auto wire = parse_depth(odd, storage);
    ASSERT_TRUE(wire.has_value() && wire->has_value());
    ASSERT_EQ(book.apply(**wire, base), DepthBook::Step::Applied);
    EXPECT_EQ(book.snapshot(base.id, k_ingest)->bids[0].qty.raw, 12345);
}

TEST(ToobitTrades, ReadsIsBuyerMakerAsASellingTaker) {
    PrintBuffer storage;
    const auto live = parse_trades(fixture("ws_trade.json"), storage);
    ASSERT_TRUE(live.has_value());
    EXPECT_FALSE(live->first);
    ASSERT_EQ(live->rows.size(), 1U);
    EXPECT_EQ(live->rows[0].id, 4900197869795462719ULL);
    const auto trade = map_trade(live->rows[0], base_for("BTC-SWAP-USDT"), k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Sell);
    EXPECT_EQ(trade->price.raw, 848001);
    EXPECT_EQ(trade->qty.raw, 95);
    EXPECT_EQ(trade->ts_event.ns, 1'790'964'249'384ULL * 1'000'000ULL);

    const auto history = parse_trades(fixture("ws_trade_snapshot.json"), storage);
    ASSERT_TRUE(history.has_value());
    EXPECT_TRUE(history->first);
    EXPECT_EQ(history->rows.size(), 60U);
}

TEST(ToobitTrades, CountsASizePastNineBillionInWholeLots) {
    WirePrint print{};
    print.id = 1;
    print.ts_ms = 1'790'964'249'384ULL;
    print.price = "0.00000419";
    print.qty = "27603593224.5";
    const auto trade = map_trade(print, base_for("PEPEUSDT", MarketType::Spot), k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->qty.raw, 27'603'593'224LL);
    EXPECT_EQ(trade->price.raw, 419);
}

TEST(ToobitMark, TruncatesTheMarkOntoTheTick) {
    const auto wire = parse_mark(fixture("ws_mark.json"));
    ASSERT_TRUE(wire.has_value() && wire->has_value());
    EXPECT_EQ((*wire)->ts_ms, 1'790'964'250'000ULL);
    const auto mark = map_mark(**wire, base_for("BTC-SWAP-USDT"));
    ASSERT_TRUE(mark.has_value());
    EXPECT_EQ(mark->raw, 848075);
}

// toobit's printed example signatures do not match their own inputs; openssl agrees with these.
TEST(ToobitSigner, MatchesTheDocumentedScheme) {
    net::Signer signer;
    signer.set_api_key("SRQGN9M8Sr87nbfKsaSxm33Y6CmGVtUu9Erz73g9vHFNn36VROOKSaWBQ8OSOtSq");
    ASSERT_TRUE(signer.secret().assign("30lfjDT51iOG1kYZnDoLNynOyMdIcmQyO1XYfxzYOmQfx9tjiI98Pzio4uhZ0Uk2").has_value());
    std::array<char, net::k_hex_bytes + 1> out{};
    EXPECT_EQ(sign_query(signer,
                         "symbol=BTC-SWAP-USDT&side=SELL&type=LIMIT&timeInForce=GTC&quantity=1&price=400&"
                         "recvWindow=100000&timestamp=1668481902307",
                         out),
              "c51bc80daaf1608a3ab11bec7e3b35a41d351e0224f46b0326447c489d790034");
    EXPECT_EQ(sign_query(signer,
                         "symbol=BTC-SWAP-USDT&side=SELL&type=LIMIT&timeInForce=GTCquantity=1&price=400&"
                         "recvWindow=10000000&timestamp=1668481902307",
                         out),
              "7abb3b48084b204592d0d6d6a620a77d210a101afd8d4786446b27ac9d4c9d45");

    net::Signer reference;
    reference.set_api_key("vmPUZE6mv9SD5VNHk4HlWFsOr6aKE2zvsw0MuIgwCIPy6utIco14y7Ju91duEh8A");
    ASSERT_TRUE(
        reference.secret().assign("NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j").has_value());
    EXPECT_EQ(sign_query(reference,
                         "symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC&quantity=1&price=0.1&"
                         "recvWindow=5000&timestamp=1499827319559",
                         out),
              "c8db56825ae71d6d79447849e617115f4a920fa2acdcab2b053c4b2838bd6b71");
}

TEST(ToobitRequests, SignsTheQueryItSendsAndRefusesWithoutKeys) {
    net::Signer signer;
    RequestBuilder unkeyed{signer};
    OrderCmd cmd{};
    cmd.instrument = base_for("BTC-SWAP-USDT").id;
    cmd.side = Side::Sell;
    cmd.flags = OrderFlags::ReduceOnly;
    cmd.qty = QtyLots::from_raw(5);
    cmd.price = PriceTicks::from_raw(848001);
    cmd.exchange_client_order_id = coid();
    const UnixNanos now{1'668'481'902'307ULL * 1'000'000ULL};
    const auto refused = unkeyed.create_order(cmd, base_for("BTC-SWAP-USDT"), now);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), BuildError::NoCredentials);

    signer.set_api_key("key");
    ASSERT_TRUE(signer.secret().assign("secret").has_value());
    RequestBuilder builder{signer};
    const auto request = builder.create_order(cmd, base_for("BTC-SWAP-USDT"), now);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(builder.last_signed(),
              "symbol=BTC-SWAP-USDT&side=SELL_CLOSE&type=LIMIT&quantity=0.5&price=84800.1&priceType=INPUT&"
              "timeInForce=GTC&newClientOrderId=01HZY3M7K8Q9R0S1T2V3W4X5Y6&recvWindow=5000&"
              "timestamp=1668481902307");
    std::array<char, net::k_hex_bytes + 1> out{};
    const auto signature = sign_query(signer, builder.last_signed(), out);
    EXPECT_EQ(request->target,
              std::string{ep::k_order} + "?" + std::string{builder.last_signed()} +
                  "&signature=" + std::string{signature});
    ASSERT_EQ(request->headers.size(), 1U);
    EXPECT_EQ(request->headers[0].name, ep::k_api_key_header);
    EXPECT_EQ(request->method, net::HttpMethod::Post);

    cmd.order_type = OrderType::Market;
    cmd.flags = OrderFlags::None;
    cmd.side = Side::Buy;
    ASSERT_TRUE(builder.create_order(cmd, base_for("BTC-SWAP-USDT"), now).has_value());
    EXPECT_TRUE(builder.last_signed().starts_with(
        "symbol=BTC-SWAP-USDT&side=BUY_OPEN&type=LIMIT&quantity=0.5&priceType=MARKET&"));

    const auto cancel = builder.cancel_order(cmd, now);
    ASSERT_TRUE(cancel.has_value());
    EXPECT_EQ(cancel->method, net::HttpMethod::Delete);
    EXPECT_TRUE(builder.last_signed().starts_with("origClientOrderId=01HZY3M7K8Q9R0S1T2V3W4X5Y6&"));
}

TEST(ToobitErrors, ReadsTheCodeAndTreatsACodelessBodyAsSuccess) {
    const auto err = parse_rest_error(fixture("rest_error.json"));
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, -1107);
    EXPECT_TRUE(err->msg.starts_with("The accessKey"));
    const auto ok = parse_rest_error(fixture("rest_order.json"));
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->code, 0);
    EXPECT_EQ(parse_rest_error(fixture("rest_open_orders.json"))->code, 0);
}

TEST(ToobitUserStream, MapsAnExecutionReport) {
    ExecutionBuffer storage;
    const auto frame = parse_private(fixture("user_execution.json"), storage);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->kind, PrivateKind::Events);
    ASSERT_EQ(frame->executions.size(), 1U);
    const auto event = map_execution(frame->executions[0], table(), k_ingest);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::Accepted);
    EXPECT_EQ(event->side, Side::Buy);
    EXPECT_TRUE(has_flag(event->flags, OrderFlags::ReduceOnly));
    EXPECT_EQ(event->exchange_order_id.view(), "4293153");
    EXPECT_EQ(event->exchange_client_order_id.view(), "1000087761");
    EXPECT_EQ(event->qty.raw, 10);
    EXPECT_EQ(event->ts_event.ns, 1'499'405'658'658ULL * 1'000'000ULL);

    const std::string fill{R"([{"e":"contractExecutionReport","E":1790964249700,"s":"BTC-SWAP-USDT",)"
                           R"("c":"01HZY3M7K8Q9R0S1T2V3W4X5Y6","C":false,"S":"SELL","o":"LIMIT","f":"POST_ONLY",)"
                           R"("q":"1","p":"84800.1","X":"PARTIALLY_FILLED","i":"2124135487564299264","l":"0.5",)"
                           R"("z":"0.5","L":"84800.1","n":"0.0127","m":true,"U":1790964249698}])"};
    const auto filled = parse_private(fill, storage);
    ASSERT_TRUE(filled.has_value());
    ASSERT_EQ(filled->executions.size(), 1U);
    const auto trade = map_execution(filled->executions[0], table(), k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->type, OrderEventType::Filled);
    EXPECT_EQ(trade->side, Side::Sell);
    EXPECT_TRUE(has_flag(trade->flags, OrderFlags::PostOnly));
    EXPECT_EQ(trade->qty.raw, 5);
    EXPECT_EQ(trade->leaves_qty.raw, 5);
    EXPECT_EQ(trade->price.raw, 848001);
    EXPECT_EQ(trade->liquidity, Liquidity::Maker);
    EXPECT_EQ(trade->fee, e9("0.0127"));
    EXPECT_EQ(trade->exchange_client_order_id, coid());
    EXPECT_EQ(trade->exec_id.view(), "2124135487564299264-0.5");

    const auto expiring = parse_private(fixture("user_listen_key_expiring.json"), storage);
    ASSERT_TRUE(expiring.has_value());
    EXPECT_EQ(expiring->kind, PrivateKind::ListenKeyExpiring);
    EXPECT_EQ(parse_listen_key(fixture("rest_listen_key.json")).value_or(""),
              "1A9LWJjuMwKWYP4QQPw34GRm8gz3x5AephXSuqcDef1RnzoBVhEeGE963CoS1Sgj");
}

TEST(ToobitReconciliation, MapsOrdersFillsPositionsAndBalances) {
    const auto order = parse_order(fixture("rest_order.json"));
    ASSERT_TRUE(order.has_value());
    const auto report = map_order(*order, table());
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->status, OrderStatus::Accepted);
    EXPECT_EQ(report->side, Side::Buy);
    EXPECT_EQ(report->price.raw, 190000);
    EXPECT_EQ(report->qty.raw, 100);
    EXPECT_EQ(report->leaves.raw, 100);
    EXPECT_EQ(report->exchange_order_id.view(), "1289182123551455488");

    const auto orders_storage = std::make_unique<OrderBuffer>();
    const auto open = parse_orders(fixture("rest_open_orders.json"), *orders_storage);
    ASSERT_TRUE(open.has_value());
    ASSERT_EQ(open->size(), 1U);
    EXPECT_EQ(map_order(open->front(), table())->qty.raw, 2220);

    const auto trades_storage = std::make_unique<UserTradeBuffer>();
    const auto trades = parse_user_trades(fixture("rest_user_trades.json"), *trades_storage);
    ASSERT_TRUE(trades.has_value());
    const auto fill = map_user_trade(trades->front(), table());
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->qty.raw, 90);
    EXPECT_EQ(fill->price.raw, 240000);
    EXPECT_EQ(fill->fee, e9("0.0162"));
    EXPECT_EQ(fill->liquidity, Liquidity::Taker);
    EXPECT_EQ(fill->exec_id.view(), "1289239136943831296");

    const auto positions_storage = std::make_unique<PositionBuffer>();
    const auto positions = parse_positions(fixture("rest_positions.json"), *positions_storage);
    ASSERT_TRUE(positions.has_value());
    const auto nets = map_positions(*positions, table(), k_ingest);
    ASSERT_EQ(nets.size(), 1U);
    EXPECT_EQ(nets[0].net_qty.raw, 100);
    EXPECT_EQ(nets[0].avg_entry.raw, 240000);

    const std::array<WirePosition, 2> hedged{
        WirePosition{.symbol = "BTC-SWAP-USDT", .side = "LONG", .avg_price = "84000", .position = "3"},
        WirePosition{.symbol = "BTC-SWAP-USDT", .side = "SHORT", .avg_price = "85000", .position = "5"}};
    const auto net = map_positions(hedged, table(), k_ingest);
    ASSERT_EQ(net.size(), 1U);
    EXPECT_EQ(net[0].net_qty.raw, -20);

    BalanceBuffer balance_storage{};
    const auto balances = parse_balances(fixture("rest_balance.json"), balance_storage);
    ASSERT_TRUE(balances.has_value());
    const auto balance = map_balance(balances->front(), k_ingest);
    ASSERT_TRUE(balance.has_value());
    EXPECT_EQ(balance->asset, *to_asset_code("USDT"));
    EXPECT_EQ(balance->unrealized, e9("10.01"));
}

TEST(ToobitCaps, StatesRestOrderEntryAndAListenKey) {
    const auto& caps = exec::caps_of(VenueId::Toobit);
    EXPECT_EQ(caps.venue, VenueId::Toobit);
    EXPECT_TRUE(caps.markets.perp);
    EXPECT_TRUE(caps.markets.spot);
    EXPECT_FALSE(caps.ws_order_entry);
    EXPECT_FALSE(caps.fix_order_entry);
    EXPECT_TRUE(caps.amend_order);
    EXPECT_EQ(caps.user_stream, exec::UserStreamModel::ListenKey);
    EXPECT_EQ(to_string(VenueId::Toobit), "toobit");
    EXPECT_EQ(static_cast<int>(VenueId::Toobit), 33);
}

} // namespace
} // namespace raikiri::venues::toobit
