#include <raikiri/domain/asset_class.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/xt/account.hpp>
#include <raikiri/venues/xt/book.hpp>
#include <raikiri/venues/xt/catalogue.hpp>
#include <raikiri/venues/xt/client_id.hpp>
#include <raikiri/venues/xt/envelope.hpp>
#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/venues/xt/map.hpp>
#include <raikiri/venues/xt/market_data.hpp>
#include <raikiri/venues/xt/orders.hpp>
#include <raikiri/venues/xt/positions.hpp>
#include <raikiri/venues/xt/rest.hpp>
#include <raikiri/venues/xt/ws.hpp>
#include <raikiri/venues/xt/xt_venue.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace raikiri::venues::xt {
namespace {

// levels between 50 and 200 are refused with levels error
constexpr std::uint32_t k_xt_depth = 50;

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream file{path};
    EXPECT_TRUE(file.is_open()) << "missing fixture " << path;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// reserve before the first add: lookup keys are views into the table's storage
void load_perp_table(exec::InstrumentTable& table) {
    static thread_local InstrumentBuffer storage;
    const auto body = fixture("rest_symbol_list.json");
    const auto wire = parse_instruments(body, storage);
    ASSERT_TRUE(wire.has_value());
    table.reserve(wire->size());
    for (const auto& entry : *wire) {
        if (const auto instrument = map_instrument(entry, MarketType::Perp); instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value());
        }
    }
}

// separate tables: spot and perp share the btc_usdt symbol
void load_spot_table(exec::InstrumentTable& table) {
    static thread_local SpotSymbolBuffer storage;
    const auto body = fixture("rest_spot_symbol_list.json");
    const auto wire = parse_spot_symbols(body, storage);
    ASSERT_TRUE(wire.has_value());
    table.reserve(wire->size());
    for (const auto& entry : *wire) {
        if (const auto instrument = map_spot_instrument(entry); instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value());
        }
    }
}

TEST(XtSpotWire, TheSnapshotIdIsANumberOnSpotAndDigitsOnFutures) {
    LevelBuffer levels;
    const auto body = fixture("ws_spot_depth.json");

    EXPECT_FALSE(parse_depth(body, levels, MarketType::Perp).has_value());

    const auto wire = parse_depth(body, levels, MarketType::Spot);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "btc_usdt");
    EXPECT_EQ(wire->update_id, 1'786'684'245'120ULL);
    EXPECT_EQ(wire->ts_ms, 0U) << "spot t repeats the update id";
    ASSERT_FALSE(wire->bids.empty());
    ASSERT_FALSE(wire->asks.empty());
    EXPECT_LT(wire->bids[0].price, wire->asks[0].price) << "an uncrossed book";
}

TEST(XtSpotWire, TheSpotAggressorIsTheOppositeOfTheFlag) {
    // spot b means the buyer was maker; futures m names the taker
    const auto body = fixture("ws_spot_public_trade.json");
    const auto wire = parse_trade(body, MarketType::Spot);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "btc_usdt");
    EXPECT_FALSE(wire->qty.empty()) << "spot names the size q, futures a";
    EXPECT_NE(wire->ts_ms, 0U);

    EXPECT_FALSE(parse_trade(body, MarketType::Perp).has_value());
}

TEST(XtSpotMap, ASpotSnapshotCrossesIntoTheDomain) {
    exec::InstrumentTable table;
    load_spot_table(table);
    LevelBuffer levels;
    const auto body = fixture("ws_spot_depth.json");
    const auto wire = parse_depth(body, levels, MarketType::Spot);
    ASSERT_TRUE(wire.has_value());

    const auto bbo = map_bbo(*wire, table, UnixNanos{7});
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->instrument.market(), MarketType::Spot);
    EXPECT_LT(bbo->bid.raw, bbo->ask.raw);
    EXPECT_EQ(bbo->ts_event.ns, 0U);
    EXPECT_EQ(bbo->ts_init, UnixNanos{7});

    const auto book = map_book(*wire, table, UnixNanos{7});
    ASSERT_TRUE(book.has_value());
    EXPECT_GT(book->bid_count, 0U);
    EXPECT_GT(book->ask_count, 0U);
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

TEST(XtVenue, RegistersAndBuilds) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto adapter = exec::make_venue({.venue = VenueId::Xt, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Xt);
    EXPECT_NE(adapter->data, nullptr);
    EXPECT_NE(adapter->execution, nullptr);
    EXPECT_NE(adapter->instruments, nullptr);
}

TEST(XtVenue, OrderWithoutAnImplementationFailsLoudly) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Xt, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.instrument = InstrumentId::make(VenueId::Xt, MarketType::Perp, SymbolId{0});
    cmd.client_order_id = ClientOrderId::make(1, 42);
    adapter->execution->submit(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);

    cmd.kind = CmdKind::Cancel;
    adapter->execution->cancel(cmd);
    ASSERT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(sink.events[1].type, OrderEventType::CancelRejected);
}

TEST(XtVenue, ExecutionPullsWithoutCredentialsAnswerAuthFailure) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Xt, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    const auto orders = adapter->execution->open_order_reports(MarketType::Perp);
    ASSERT_FALSE(orders.has_value());
    EXPECT_EQ(orders.error().cls, exec::ErrorClass::AuthFailure);

    const auto positions = adapter->execution->position_reports(MarketType::Perp);
    ASSERT_FALSE(positions.has_value());
    EXPECT_EQ(positions.error().cls, exec::ErrorClass::AuthFailure);

    const auto fills = adapter->execution->fill_reports(MarketType::Perp, UnixNanos{0});
    ASSERT_FALSE(fills.has_value());
    EXPECT_EQ(fills.error().cls, exec::ErrorClass::AuthFailure);

    const auto balances = adapter->execution->balance_reports(MarketType::Perp);
    ASSERT_FALSE(balances.has_value());
    EXPECT_EQ(balances.error().cls, exec::ErrorClass::AuthFailure);
}

// the market check must refuse before the instrument table is consulted
TEST(XtVenue, UnservedMarketsRefuseWithTheirOwnName) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Xt, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    const auto options = adapter->execution->open_order_reports(MarketType::Option);
    ASSERT_FALSE(options.has_value());
    EXPECT_EQ(options.error().cls, exec::ErrorClass::Terminal);
    EXPECT_EQ(options.error().message.view(), "xt options order entry");

    const auto dated = adapter->execution->fill_reports(MarketType::Future, UnixNanos{0});
    ASSERT_FALSE(dated.has_value());
    EXPECT_EQ(dated.error().message.view(), "xt dated futures order entry");

    EXPECT_FALSE(adapter->execution->position_reports(MarketType::Option).has_value());
    const auto balances = adapter->execution->balance_reports(MarketType::Future);
    ASSERT_FALSE(balances.has_value());
    EXPECT_EQ(balances.error().message.view(), "xt dated futures order entry");
    const auto query = adapter->execution->query_order(ExchangeClientOrderId{}, MarketType::Option);
    ASSERT_FALSE(query.has_value());
    EXPECT_EQ(query.error().message.view(), "xt options order entry");

    EXPECT_EQ(market_refusal(MarketType::Option), BuildError::OptionsUnsupported);
    EXPECT_EQ(market_refusal(MarketType::Future), BuildError::DatedFuturesUnsupported);
    EXPECT_FALSE(market_refusal(MarketType::Spot).has_value());
    EXPECT_FALSE(market_refusal(MarketType::Perp).has_value());
}

TEST(XtVenue, SpotPositionPullSaysSoRatherThanAnsweringFlat) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Xt, .market = MarketType::Spot}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    const auto positions = adapter->execution->position_reports(MarketType::Spot);
    ASSERT_FALSE(positions.has_value());
    EXPECT_EQ(positions.error().cls, exec::ErrorClass::Terminal);
}

TEST(XtVenue, SpotBalancePullNamesItselfRatherThanAnsweringWithTheFuturesWallet) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Xt, .market = MarketType::Spot}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    const auto balances = adapter->execution->balance_reports(MarketType::Spot);
    ASSERT_FALSE(balances.has_value());
    EXPECT_EQ(balances.error().cls, exec::ErrorClass::Terminal);
    EXPECT_EQ(balances.error().message.view(), exec::not_implemented("xt spot balance pull").message.view());
}

TEST(XtWire, EventKindComesFromTopic) {
    const auto depth_body = fixture("ws_depth.json");
    const auto depth = parse_event(depth_body);
    ASSERT_TRUE(depth.has_value());
    EXPECT_EQ(depth->kind, EventKind::Depth);
    EXPECT_EQ(depth->symbol, "btc_usdt");

    const auto update_body = fixture("ws_depth_update.json");
    const auto update = parse_event(update_body);
    ASSERT_TRUE(update.has_value());
    EXPECT_EQ(update->kind, EventKind::DepthUpdate);

    const auto trade_body = fixture("ws_trade_buy.json");
    const auto trade = parse_event(trade_body);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->kind, EventKind::Trade);

    const auto mark_body = fixture("ws_mark_price.json");
    const auto mark = parse_event(mark_body);
    ASSERT_TRUE(mark.has_value());
    EXPECT_EQ(mark->kind, EventKind::MarkPrice);

    // agg_ticker cannot fill a Bbo
    const auto agg_body = fixture("ws_agg_ticker.json");
    const auto agg = parse_event(agg_body);
    ASSERT_TRUE(agg.has_value());
    EXPECT_EQ(agg->kind, EventKind::Unknown);
}

TEST(XtWire, ControlFramesSplitOnCode) {
    const auto ack_body = fixture("ws_subscribe_ack.json");
    const auto ack = parse_event(ack_body);
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->kind, EventKind::Ack);
    EXPECT_TRUE(ack->symbol.empty());

    const auto refused_body = fixture("ws_error_unknown_topic.json");
    const auto refused = parse_event(refused_body);
    ASSERT_TRUE(refused.has_value());
    EXPECT_EQ(refused->kind, EventKind::Error);
}

TEST(XtWire, FundingTopicIsRefusedByTheVenue) {
    const auto body = fixture("ws_error_unknown_topic.json");
    const auto err = parse_stream_ack(body);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, 1);
    EXPECT_NE(err->msg.find("funding_rate"), std::string_view::npos);
    EXPECT_EQ(err->id, "nofunding");
}

// simdjson reads the bare word as a lazy scalar, so this fails as WrongType
TEST(XtWire, NonJsonFrameIsIgnoredNotFatal) {
    const auto event = parse_event("Invalid parameter");
    ASSERT_FALSE(event.has_value());
    EXPECT_EQ(event.error(), WireError::WrongType);
    EXPECT_FALSE(parse_event("").has_value());
}

TEST(XtWire, DepthSnapshotParsesTopOfBook) {
    const auto body = fixture("ws_depth.json");
    LevelBuffer levels;
    const auto wire = parse_depth(body, levels, MarketType::Perp);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "btc_usdt");
    EXPECT_EQ(wire->bids.size(), k_xt_depth);
    EXPECT_EQ(wire->asks.size(), k_xt_depth);
    EXPECT_GT(wire->update_id, 0U);
    EXPECT_GT(wire->ts_ms, 0U);
    const auto price = [](std::string_view text) { return parse_decimal_e9(text)->raw; };
    EXPECT_LT(price(wire->bids[0].price), price(wire->asks[0].price));
    EXPECT_GT(price(wire->bids[0].price), price(wire->bids[1].price));
    EXPECT_LT(price(wire->asks[0].price), price(wire->asks[1].price));
}

TEST(XtWire, DepthUpdateChainsOnPrevUpdateId) {
    const auto first_body = fixture("ws_depth_update.json");
    const auto next_body = fixture("ws_depth_update_next.json");
    LevelBuffer first_levels;
    LevelBuffer next_levels;
    const auto first = parse_depth_update(first_body, first_levels);
    const auto next = parse_depth_update(next_body, next_levels);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->prev_update_id, first->update_id);
    EXPECT_EQ(next->first_update_id, next->prev_update_id + 1);
}

// Getting this backwards inverts every signed-flow series built on it, so the
// direction is pinned to the live correlation that established it.
TEST(XtWire, TradeSideIsTheAggressorNotTheMaker) {
    const auto buy_body = fixture("ws_trade_buy.json");
    const auto bought = parse_trade(buy_body, MarketType::Perp);
    ASSERT_TRUE(bought.has_value());
    EXPECT_TRUE(bought->aggressor_bought); // m == "BID": taker lifted the ask

    const auto sell_body = fixture("ws_trade_sell.json");
    const auto sold = parse_trade(sell_body, MarketType::Perp);
    ASSERT_TRUE(sold.has_value());
    EXPECT_FALSE(sold->aggressor_bought); // m == "ASK": taker hit the bid
    EXPECT_EQ(sold->symbol, "btc_usdt");
    EXPECT_GT(sold->ts_ms, 0U);
}

TEST(XtWire, UnknownTradeSideIsRefused) {
    const auto frame =
        R"({"topic":"trade","event":"trade@btc_usdt","data":{"s":"btc_usdt","p":"1.0","a":"2","m":"MID","t":1786394642785}})";
    const auto wire = parse_trade(frame, MarketType::Perp);
    ASSERT_FALSE(wire.has_value());
    EXPECT_EQ(wire.error(), WireError::WrongType);
}

TEST(XtWire, MarkPriceParses) {
    const auto body = fixture("ws_mark_price.json");
    const auto wire = parse_mark_price(body);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "btc_usdt");
    EXPECT_FALSE(wire->mark_price.empty());
    EXPECT_GT(wire->ts_ms, 0U);
}

TEST(XtWire, MalformedFrameIsAnError) {
    EXPECT_FALSE(parse_trade(R"({"topic":"trade","event":"x","data":{"s":"btc_usdt")", MarketType::Perp).has_value());
    LevelBuffer levels;
    EXPECT_FALSE(parse_depth(R"({"topic":"depth","data":{"s":"btc_usdt"}})", levels, MarketType::Perp).has_value());
}

TEST(XtRest, EnvelopeReportsSuccessAndFailureSeparately) {
    const auto ok_body = fixture("rest_server_time.json");
    const auto ok = parse_rest_envelope(ok_body);
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->return_code, k_ok);
    EXPECT_TRUE(ok->code.empty()); // `error` is null on success
    EXPECT_EQ(ok->msg, "success");

    const auto bad_symbol_body = fixture("rest_error_invalid_symbol.json");
    const auto bad_symbol = parse_rest_envelope(bad_symbol_body);
    ASSERT_TRUE(bad_symbol.has_value());
    EXPECT_EQ(bad_symbol->return_code, 1);
    EXPECT_EQ(bad_symbol->code, k_invalid_symbol);
    EXPECT_EQ(bad_symbol->msg, "invalid symbol");

    const auto bad_param_body = fixture("rest_error_invalid_param.json");
    const auto bad_param = parse_rest_envelope(bad_param_body);
    ASSERT_TRUE(bad_param.has_value());
    EXPECT_EQ(bad_param->code, k_invalid_param);
}

TEST(XtRest, UnreadableEnvelopeNeverDecaysIntoSuccess) {
    EXPECT_FALSE(parse_rest_envelope(R"({"msgInfo":"success"})").has_value());
    EXPECT_FALSE(parse_rest_envelope("not json at all").has_value());
}

TEST(XtRest, ServerTimeIsABareNumberInResult) {
    const auto body = fixture("rest_server_time.json");
    const auto ms = parse_server_time(body);
    ASSERT_TRUE(ms.has_value());
    EXPECT_GT(*ms, 1'577'836'800'000ULL);
}

TEST(XtRest, FundingRateReadsANumberField) {
    const auto body = fixture("rest_funding_rate.json");
    const auto wire = parse_funding_rate(body);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "btc_usdt");
    EXPECT_EQ(wire->rate_token, "0.00008866");
    EXPECT_EQ(wire->interval_h, 8); // collectionInternal counts hours
    EXPECT_GT(wire->next_collection_ms, 0U);
}

TEST(XtRest, RequestTargetsCarryTheSymbolVerbatim) {
    RequestBuilder builder;
    EXPECT_EQ(builder.server_time().target, "/future/market/v1/public/time");
    EXPECT_EQ(builder.symbol_list().target, "/future/market/v1/public/symbol/list");
    EXPECT_EQ(builder.funding_rate("btc_usdt").target, "/future/market/v1/public/q/funding-rate?symbol=btc_usdt");
    EXPECT_EQ(builder.depth("eth_usdt", 5).target, "/future/market/v1/public/q/depth?symbol=eth_usdt&level=5");
}

TEST(XtErrors, VerifiedCodesClassifyAndUnknownIsTerminal) {
    EXPECT_EQ(classify(k_invalid_symbol), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_invalid_param), exec::ErrorClass::Terminal);
    // 400 also covers malformed requests, so it must not halt the venue
    EXPECT_EQ(classify("400"), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify("something_xt_added_later"), exec::ErrorClass::Terminal);

    EXPECT_EQ(classify("401"), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify("403"), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify("405"), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify("429"), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify("503"), exec::ErrorClass::Retryable);
    // GATEWAY_0006 and _0007 lock the account; retrying extends the lock
    EXPECT_EQ(classify("GATEWAY_0003"), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify("GATEWAY_0006"), exec::ErrorClass::RateLimited);
}

TEST(XtErrors, SpotCodesAreTheirOwnVocabulary) {
    EXPECT_EQ(classify_spot(k_spot_missing_key), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify_spot("AUTH_103"), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify_spot("AUTH_105"), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify_spot("GATEWAY_0001"), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify_spot("COMMON_002"), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify_spot("ORDER_002"), exec::ErrorClass::Terminal); // insufficient funds
    EXPECT_EQ(classify_spot("SYMBOL_005"), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify_spot("something_xt_added_later"), exec::ErrorClass::Terminal);

    EXPECT_EQ(classify("AUTH_103"), exec::ErrorClass::Terminal);
}

TEST(XtErrors, OnlyAMissingOrderMakesACancelFailureBenign) {
    EXPECT_TRUE(is_benign_cancel_failure("ORDER_005"));
    EXPECT_TRUE(is_benign_cancel_failure("invalid_order"));
    EXPECT_FALSE(is_benign_cancel_failure("invalid_params"));
    EXPECT_FALSE(is_benign_cancel_failure("ORDER_002"));
}

TEST(XtErrors, LiveRefusalBodiesCarryTheirCode) {
    const auto spot_body = fixture("rest_spot_error_auth.json");
    const auto spot = parse_spot_envelope(spot_body);
    ASSERT_TRUE(spot.has_value());
    EXPECT_NE(spot->rc, k_spot_ok);
    EXPECT_EQ(spot->mc, k_spot_missing_key);
    EXPECT_EQ(classify_spot(spot->mc), exec::ErrorClass::AuthFailure);

    const auto futures_body = fixture("rest_futures_error_no_key.json");
    const auto futures = parse_rest_envelope(futures_body);
    ASSERT_TRUE(futures.has_value());
    EXPECT_NE(futures->return_code, k_ok);
    EXPECT_EQ(futures->code, "400");

    // 404 is the one spot refusal with a non-200 status; mc carries prose there
    const auto missing_body = fixture("rest_spot_error_not_found.json");
    const auto missing = parse_spot_envelope(missing_body);
    ASSERT_TRUE(missing.has_value());
    EXPECT_NE(missing->rc, k_spot_ok);
}

TEST(XtErrors, TextCodesSurviveAsZeroRatherThanGuessing) {
    EXPECT_EQ(numeric_code("400"), 400);
    EXPECT_EQ(numeric_code(k_invalid_symbol), 0);
    EXPECT_EQ(numeric_code(""), 0);
}

TEST(XtWs, TopicsMatchWhatTheVenueAccepts) {
    EXPECT_EQ(topic_for(exec::SubKind::Bbo, MarketType::Perp).channel, "depth");
    EXPECT_EQ(topic_for(exec::SubKind::Bbo, MarketType::Perp).params, ",20,100ms");
    EXPECT_EQ(topic_for(exec::SubKind::Trades, MarketType::Perp).channel, "trade");
    EXPECT_EQ(topic_for(exec::SubKind::Depth, MarketType::Perp).channel, "depth");
    EXPECT_EQ(topic_for(exec::SubKind::MarkPrice, MarketType::Perp).channel, "mark_price");
    EXPECT_TRUE(topic_for(exec::SubKind::Funding, MarketType::Perp).channel.empty());
    EXPECT_TRUE(topic_for(exec::SubKind::Kline, MarketType::Perp).channel.empty());
}

TEST(XtWs, SubscribeFrameJoinsTopicWithAtSign) {
    Protocol protocol;
    const std::array<Subscription, 2> subs{
        Subscription{.channel = "depth", .symbol = "btc_usdt", .params = ",50,100ms"},
        Subscription{.channel = "trade", .symbol = "eth_usdt", .params = {}}};
    const auto frame = protocol.subscribe_frame(subs);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->consumed, 2U);
    EXPECT_EQ(frame->text, R"({"method":"subscribe","params":["depth@btc_usdt,50,100ms","trade@eth_usdt"],"id":"1"})");

    // the id advances per frame
    const auto next = protocol.unsubscribe_frame(std::span{subs}.first(1));
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->text, R"({"method":"unsubscribe","params":["depth@btc_usdt,50,100ms"],"id":"2"})");
}

TEST(XtWs, DeclinedKindNeverReachesTheWire) {
    Protocol protocol;
    const std::array<Subscription, 1> empty_channel{Subscription{.channel = {}, .symbol = "btc_usdt", .params = {}}};
    const auto frame = protocol.subscribe_frame(empty_channel);
    ASSERT_FALSE(frame.has_value());
    EXPECT_EQ(frame.error(), WsBuildError::UnsupportedKind);
    EXPECT_FALSE(protocol.subscribe_frame({}).has_value());
}

TEST(XtMap, StepFromPrecisionIsAnIntegerPowerOfTen) {
    EXPECT_EQ(step_from_precision(0)->raw, k_e9);       // 1
    EXPECT_EQ(step_from_precision(2)->raw, 10'000'000); // 0.01
    EXPECT_EQ(step_from_precision(9)->raw, 1);          // 1e-9
    EXPECT_FALSE(step_from_precision(10).has_value());
    EXPECT_FALSE(step_from_precision(-1).has_value());
}

TEST(XtMap, CatalogueAcceptsOnlyTradablePerps) {
    static thread_local InstrumentBuffer storage;
    const auto body = fixture("rest_symbol_list.json");
    const auto wire = parse_instruments(body, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->size(), 5U);

    std::vector<std::string_view> accepted;
    for (const auto& entry : *wire) {
        if (map_instrument(entry, MarketType::Perp).has_value()) {
            accepted.push_back(entry.symbol);
        }
    }
    // ftt_usdt has tradeSwitch=false, dia_usdt isOpenApi=false, btc_usdt_261225 is dated
    EXPECT_EQ(accepted, (std::vector<std::string_view>{"btc_usdt", "eth_usdt"}));
}

TEST(XtMap, PerpCarriesVenueStatedStepsAndContractSize) {
    static thread_local InstrumentBuffer storage;
    const auto body = fixture("rest_symbol_list.json");
    const auto wire = parse_instruments(body, storage);
    ASSERT_TRUE(wire.has_value());

    const auto instrument = map_instrument(wire->front(), MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    ASSERT_TRUE(std::holds_alternative<PerpInstrument>(*instrument));
    const auto& base = base_of(*instrument);
    EXPECT_EQ(base.exchange_symbol.view(), "btc_usdt");
    EXPECT_EQ(base.tick_size.raw, 100'000'000);  // minStepPrice 0.1, stated
    EXPECT_EQ(base.lot_size.raw, k_e9);          // quantityPrecision 0, derived
    EXPECT_EQ(base.contract_value.raw, 100'000); // 0.0001 BTC per contract
    EXPECT_EQ(base.min_notional.raw, 10 * k_e9);
    // the funding interval comes from the funding read
    EXPECT_EQ(std::get<PerpInstrument>(*instrument).funding_interval_s, 0U);
}

TEST(XtMap, TickIsThePricePrecisionWhenItIsFinerThanTheStatedStep) {
    static thread_local InstrumentBuffer storage;
    const auto body = fixture("rest_symbol_list_twlo.json");
    const auto wire = parse_instruments(body, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->size(), 1U);
    EXPECT_EQ(wire->front().tick_size, "0.1");
    EXPECT_EQ(wire->front().price_precision, 2);
    const auto instrument = map_instrument(wire->front(), MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    EXPECT_EQ(base_of(*instrument).tick_size.raw, k_e9 / 100);

    const auto btc_body = fixture("rest_symbol_list.json");
    const auto btc = parse_instruments(btc_body, storage);
    ASSERT_TRUE(btc.has_value());
    EXPECT_EQ(base_of(*map_instrument(btc->front(), MarketType::Perp)).tick_size.raw, k_e9 / 10);
}

TEST(XtMap, SpotIsRefusedRatherThanAnsweredEmpty) {
    static thread_local InstrumentBuffer storage;
    const auto body = fixture("rest_symbol_list.json");
    const auto wire = parse_instruments(body, storage);
    ASSERT_TRUE(wire.has_value());
    for (const auto& entry : *wire) {
        EXPECT_FALSE(map_instrument(entry, MarketType::Spot).has_value());
    }
}

TEST(XtMap, TheCanonicalPairIsSplitOutOfTheLowercaseSymbol) {
    static thread_local InstrumentBuffer storage;
    const auto body = fixture("rest_symbol_list.json");
    const auto wire = parse_instruments(body, storage);
    ASSERT_TRUE(wire.has_value());
    const auto perp = map_instrument(wire->front(), MarketType::Perp);
    ASSERT_TRUE(perp.has_value());
    EXPECT_EQ(base_of(*perp).exchange_symbol.view(), "btc_usdt");
    EXPECT_EQ(base_of(*perp).local_symbol.view(), "BTC/USDT");

    static thread_local SpotSymbolBuffer spot_storage;
    const auto spot_body = fixture("rest_spot_symbol_list.json");
    const auto spot_rows = parse_spot_symbols(spot_body, spot_storage);
    ASSERT_TRUE(spot_rows.has_value());
    const auto spot = map_spot_instrument(spot_rows->front());
    ASSERT_TRUE(spot.has_value());
    EXPECT_EQ(base_of(*spot).exchange_symbol.view(), "btc_usdt");
    EXPECT_EQ(base_of(*spot).local_symbol.view(), "BTC/USDT");
}

TEST(XtMap, ARowWhosePairCannotBeReadIsDropped) {
    WireInstrument wire{};
    wire.symbol = "btc_usdt";
    wire.product_type = "perpetual";
    wire.tick_size = "0.1";
    wire.contract_size = "0.0001";
    wire.state = 0;
    wire.qty_precision = 0;
    wire.trade_switch = true;
    wire.open_api = true;
    ASSERT_TRUE(map_instrument(wire, MarketType::Perp).has_value());

    for (const char* symbol : {"btcusdt", "btc_usdt_261225", "_usdt", "btc_", ""}) {
        wire.symbol = symbol;
        EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value()) << symbol;
    }

    WireSpotSymbol row{};
    row.symbol = "btc_usdt";
    row.state = "ONLINE";
    row.price_precision = 2;
    row.qty_precision = 5;
    row.trading_enabled = true;
    row.openapi_enabled = true;
    ASSERT_TRUE(map_spot_instrument(row).has_value());
    row.symbol = "btcusdt";
    EXPECT_FALSE(map_spot_instrument(row).has_value());
}

TEST(XtMap, BboComesFromTheTopOfTheDepthSnapshot) {
    exec::InstrumentTable table;
    load_perp_table(table);
    const auto body = fixture("ws_depth.json");
    LevelBuffer levels;
    const auto wire = parse_depth(body, levels, MarketType::Perp);
    ASSERT_TRUE(wire.has_value());

    const UnixNanos received{123'456'789ULL};
    const auto bbo = map_bbo(*wire, table, received);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_LT(bbo->bid.raw, bbo->ask.raw);
    EXPECT_GT(bbo->bid_qty.raw, 0);
    EXPECT_GT(bbo->ask_qty.raw, 0);
    EXPECT_EQ(bbo->ts_init, received);
    EXPECT_NE(bbo->ts_event, received); // the venue stated its own publish time
    EXPECT_EQ(bbo->bid.raw * 100'000'000, parse_decimal_e9(wire->bids[0].price)->raw);
}

TEST(XtMap, BookSnapshotCrossesIntoTheDomain) {
    exec::InstrumentTable table;
    load_perp_table(table);
    const auto body = fixture("ws_depth.json");
    LevelBuffer levels;
    const auto wire = parse_depth(body, levels, MarketType::Perp);
    ASSERT_TRUE(wire.has_value());

    const UnixNanos received{123'456'789ULL};
    const auto book = map_book(*wire, table, received);
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->bid_count, k_xt_depth);
    EXPECT_EQ(book->ask_count, k_xt_depth);
    static_assert(k_xt_depth <= k_book_levels);
    EXPECT_EQ(book->venue_seq, wire->update_id);
    EXPECT_EQ(book->ts_init, received);

    EXPECT_LT(book->bids[0].price.raw, book->asks[0].price.raw);
    for (std::uint32_t i = 1; i < book->bid_count; ++i) {
        EXPECT_LT(book->bids[i].price.raw, book->bids[i - 1].price.raw) << i;
    }
    for (std::uint32_t i = 1; i < book->ask_count; ++i) {
        EXPECT_GT(book->asks[i].price.raw, book->asks[i - 1].price.raw) << i;
    }
    for (std::uint32_t i = 0; i < book->bid_count; ++i) {
        EXPECT_GT(book->bids[i].qty.raw, 0) << i;
    }
    for (std::uint32_t i = 0; i < book->ask_count; ++i) {
        EXPECT_GT(book->asks[i].qty.raw, 0) << i;
    }
}

TEST(XtMap, AnUnknownSymbolYieldsNoBook) {
    exec::InstrumentTable table;
    load_perp_table(table);
    const auto body = fixture("ws_depth.json");
    LevelBuffer levels;
    auto wire = parse_depth(body, levels, MarketType::Perp);
    ASSERT_TRUE(wire.has_value());
    wire->symbol = "not_listed";
    EXPECT_FALSE(map_book(*wire, table, UnixNanos{1}).has_value());
}

TEST(XtMap, UnknownSymbolAndEmptySideAreDropped) {
    exec::InstrumentTable table;
    load_perp_table(table);
    const auto body = fixture("ws_depth.json");
    LevelBuffer levels;
    auto wire = parse_depth(body, levels, MarketType::Perp);
    ASSERT_TRUE(wire.has_value());

    auto foreign = *wire;
    foreign.symbol = "not_listed";
    EXPECT_FALSE(map_bbo(foreign, table, UnixNanos{1}).has_value());

    auto empty_asks = *wire;
    empty_asks.asks = {};
    EXPECT_FALSE(map_bbo(empty_asks, table, UnixNanos{1}).has_value());
}

TEST(XtMap, TradeMapsToAggressorSide) {
    exec::InstrumentTable table;
    load_perp_table(table);
    const auto buy_body = fixture("ws_trade_buy.json");
    const auto bought = parse_trade(buy_body, MarketType::Perp);
    ASSERT_TRUE(bought.has_value());
    const auto buy = map_trade(*bought, table, UnixNanos{7});
    ASSERT_TRUE(buy.has_value());
    EXPECT_EQ(buy->aggressor, Side::Buy);
    EXPECT_GT(buy->qty.raw, 0); // a count of contracts, lot size 1

    const auto sell_body = fixture("ws_trade_sell.json");
    const auto sold = parse_trade(sell_body, MarketType::Perp);
    ASSERT_TRUE(sold.has_value());
    const auto sell = map_trade(*sold, table, UnixNanos{7});
    ASSERT_TRUE(sell.has_value());
    EXPECT_EQ(sell->aggressor, Side::Sell);
}

TEST(XtMap, PriceOffTheTickIsDroppedNotRounded) {
    exec::InstrumentTable table;
    load_perp_table(table);
    const auto body = fixture("ws_trade_buy.json");
    auto wire = parse_trade(body, MarketType::Perp);
    ASSERT_TRUE(wire.has_value());
    auto off_tick = *wire;
    off_tick.price = "64161.75"; // btc_usdt steps by 0.1
    EXPECT_FALSE(map_trade(off_tick, table, UnixNanos{1}).has_value());
}

TEST(XtMap, FundingCarriesTheIntervalInSeconds) {
    exec::InstrumentTable table;
    load_perp_table(table);
    const auto body = fixture("rest_funding_rate.json");
    const auto wire = parse_funding_rate(body);
    ASSERT_TRUE(wire.has_value());

    const UnixNanos received{555ULL};
    FundingMemo memo{};
    const auto funding = map_funding(*wire, table, memo, received);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 88'660); // 0.00008866 in 1e9 fixed point
    EXPECT_EQ(funding->interval_s, 8U * 3600U);
    EXPECT_GT(funding->next_funding_time.ns, 0ULL);
    // no publish time on this endpoint; the mark arrives on its own channel
    EXPECT_EQ(funding->ts_event.ns, 0U);
    EXPECT_TRUE(funding->mark_price.is_undef());
    EXPECT_TRUE(funding->predicted_rate.is_undef());
}

TEST(XtMap, TheIndexJoinsTheFundingRecordOffTheGrid) {
    exec::InstrumentTable table;
    load_perp_table(table);
    const auto body = fixture("ws_index_price.json");
    EXPECT_EQ(parse_event(body)->kind, EventKind::IndexPrice);
    const auto wire = parse_mark_price(body);
    ASSERT_TRUE(wire.has_value());
    FundingMemo memo{};
    const auto funding = map_index_price(*wire, table, memo, UnixNanos{1});
    ASSERT_TRUE(funding.has_value());
    const auto* btc = table.find("btc_usdt");
    ASSERT_NE(btc, nullptr);
    // 86464.0012776 truncated to the tick
    EXPECT_EQ(funding->index_price.raw, parse_decimal_e9("86464.0012776")->raw / base_of(*btc).tick_size.raw);
    EXPECT_EQ(funding->ts_event.ns, 1'791'300'193'859'000'000ULL);
    EXPECT_TRUE(funding->rate.is_undef());
}

// built from the live ledger capture's symbol
exec::InstrumentTable table_with_flock() {
    exec::InstrumentTable table;
    WireInstrument wire{};
    wire.symbol = "flock_usdt";
    wire.product_type = "perpetual";
    wire.tick_size = "0.0001";
    wire.contract_size = "1";
    wire.state = 0;
    wire.qty_precision = 0;
    wire.trade_switch = true;
    wire.open_api = true;
    const auto instrument = map_instrument(wire, MarketType::Perp);
    EXPECT_TRUE(instrument.has_value());
    if (instrument.has_value()) {
        EXPECT_TRUE(table.add(*instrument).has_value());
    }
    return table;
}

TEST(XtLedger, FundingAndFeesAreBookedAndRealisedPnlIsNot) {
    const auto table = table_with_flock();
    const auto body = fixture("rest_futures_bills.json");
    BillBuffer storage;
    const auto bills = parse_bills(body, storage);
    ASSERT_TRUE(bills.has_value());
    ASSERT_EQ(bills->size(), 4U) << "the capture holds FUND x2, FEE and CLOSE_POSITION";

    std::vector<LedgerEntry> booked;
    for (const auto& wire : *bills) {
        if (const auto charge = map_bill(wire, table, UnixNanos{7}); charge.has_value()) {
            booked.push_back(*charge);
        }
    }
    ASSERT_EQ(booked.size(), 3U);
    EXPECT_EQ(booked[0].kind, LedgerKind::Funding);
    EXPECT_EQ(booked[1].kind, LedgerKind::Funding);
    EXPECT_EQ(booked[2].kind, LedgerKind::Fee);
}

TEST(XtLedger, TheAmountIsTakenVerbatimBecauseXtAlreadySignedIt) {
    // side restates the sign already on amount; applying it would flip every SUB row
    const auto table = table_with_flock();
    const auto body = fixture("rest_futures_bills.json");
    BillBuffer storage;
    const auto bills = parse_bills(body, storage);
    ASSERT_TRUE(bills.has_value());

    const auto received = map_bill((*bills)[0], table, UnixNanos{7});
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ((*bills)[0].side, "ADD");
    EXPECT_EQ(received->amount.raw, 2'100'000) << "0.0021 received, positive";

    const auto paid = map_bill((*bills)[1], table, UnixNanos{7});
    ASSERT_TRUE(paid.has_value());
    EXPECT_EQ((*bills)[1].side, "SUB");
    EXPECT_EQ(paid->amount.raw, -13'700'000) << "0.0137 paid away, and NOT flipped back";
    EXPECT_EQ(paid->settled_at.ns, 1788652805992ULL * 1'000'000ULL);
    // the venue's row id distinguishes settlements with equal stamps
    EXPECT_EQ(paid->reference.view(), "666688001376165376");
}

TEST(XtLedger, ARowNamingAnInstrumentWeDoNotHoldIsDroppedRatherThanGuessed) {
    exec::InstrumentTable empty;
    const auto body = fixture("rest_futures_bills.json");
    BillBuffer storage;
    const auto bills = parse_bills(body, storage);
    ASSERT_TRUE(bills.has_value());
    for (const auto& wire : *bills) {
        EXPECT_FALSE(map_bill(wire, empty, UnixNanos{7}).has_value());
    }
}

TEST(XtMap, TheMarkArrivesOnItsOwnChannelAndJoinsTheSameRecord) {
    exec::InstrumentTable table;
    load_perp_table(table);
    FundingMemo memo{};

    const auto rate_body = fixture("rest_funding_rate.json");
    const auto rate = parse_funding_rate(rate_body);
    ASSERT_TRUE(rate.has_value());
    const auto first = map_funding(*rate, table, memo, UnixNanos{1});
    ASSERT_TRUE(first.has_value());
    EXPECT_TRUE(first->mark_price.is_undef()) << "no mark has been stated yet";

    const auto mark_frame = fixture("ws_mark_price.json");
    const auto mark = parse_mark_price(mark_frame);
    ASSERT_TRUE(mark.has_value());
    const auto both = map_mark_price(*mark, table, memo, UnixNanos{2});
    ASSERT_TRUE(both.has_value());
    EXPECT_EQ(both->mark_price.raw, 640'747); // 64074.7 on a 0.1 tick
    EXPECT_EQ(both->rate.raw, 88'660) << "the rate the poll stated is carried forward";
    EXPECT_EQ(both->interval_s, 8U * 3600U);
}

TEST(XtMap, AFundingPollDoesNotEraseAMarkAlreadyStated) {
    exec::InstrumentTable table;
    load_perp_table(table);
    FundingMemo memo{};

    const auto mark_frame = fixture("ws_mark_price.json");
    const auto mark = parse_mark_price(mark_frame);
    ASSERT_TRUE(mark.has_value());
    ASSERT_TRUE(map_mark_price(*mark, table, memo, UnixNanos{1}).has_value());
    const auto held = memo.mark;

    const auto rate_body = fixture("rest_funding_rate.json");
    const auto rate = parse_funding_rate(rate_body);
    ASSERT_TRUE(rate.has_value());
    const auto after = map_funding(*rate, table, memo, UnixNanos{2});
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->mark_price.raw, held.raw) << "the mark survives a poll that omitted it";
}

TEST(XtMap, NegativeAndPaddedFundingRatesParse) {
    exec::InstrumentTable table;
    load_perp_table(table);
    const auto body = fixture("rest_funding_rate.json");
    auto wire = parse_funding_rate(body);
    ASSERT_TRUE(wire.has_value());
    FundingMemo memo{};
    auto padded = *wire;
    padded.rate_token = "-0.000002000000000000";
    const auto funding = map_funding(padded, table, memo, UnixNanos{1});
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, -2'000);

    auto too_fine = *wire;
    too_fine.rate_token = "0.0000000000123";
    EXPECT_FALSE(map_funding(too_fine, table, memo, UnixNanos{1}).has_value());
}

TEST(XtMap, VenueTimestampBeatsIngestTimeButZeroDoesNot) {
    const UnixNanos fallback{42ULL};
    EXPECT_EQ(ms_to_unix(0, fallback), fallback);
    EXPECT_EQ(ms_to_unix(1'786'394'629'290ULL, fallback).ns, 1'786'394'629'290'000'000ULL);
}

TEST(XtClientId, EncodingIsLosslessAndFitsTheVenueField) {
    const auto ulid = *ExchangeClientOrderId::from("01ARZ3NDEKTSV4RRFFQ69G5FAV");
    const auto encoded = encode_client_id(ulid);
    ASSERT_TRUE(encoded.has_value());
    EXPECT_EQ(encoded->view().size(), k_client_id_len);
    EXPECT_EQ(encoded->view(), "02WP8qFtGJTkHk10j5u4aZ");

    const auto decoded = decode_client_id(encoded->view());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->view(), ulid.view());
}

TEST(XtClientId, EveryCharacterIsOneTheVenueAccepts) {
    UlidGen gen{7};
    for (int i = 0; i < 4096; ++i) {
        const auto ulid = gen.next(UnixNanos{std::uint64_t{1'786'394'629'290'000'000} + static_cast<std::uint64_t>(i)});
        const auto encoded = encode_client_id(ulid);
        ASSERT_TRUE(encoded.has_value()) << ulid.view();
        ASSERT_EQ(encoded->view().size(), k_client_id_len);
        for (const char c : encoded->view()) {
            const bool legal = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
            ASSERT_TRUE(legal) << "illegal clientOrderId byte: " << c;
        }
        const auto decoded = decode_client_id(encoded->view());
        ASSERT_TRUE(decoded.has_value());
        ASSERT_EQ(decoded->view(), ulid.view());
    }
}

// both ends of the 128-bit range
TEST(XtClientId, TheBoundsOfTheUlidSpaceRoundTrip) {
    for (const auto* text : {"00000000000000000000000000", "7ZZZZZZZZZZZZZZZZZZZZZZZZZ"}) {
        const auto ulid = *ExchangeClientOrderId::from(text);
        const auto encoded = encode_client_id(ulid);
        ASSERT_TRUE(encoded.has_value()) << text;
        const auto decoded = decode_client_id(encoded->view());
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(decoded->view(), ulid.view());
    }
    EXPECT_EQ(encode_client_id(*ExchangeClientOrderId::from("00000000000000000000000000"))->view(),
              "0000000000000000000000");
}

TEST(XtClientId, ForeignIdsDecodeToNothingRatherThanToALookalike) {
    EXPECT_FALSE(decode_client_id("myOrder123").has_value());
    EXPECT_FALSE(decode_client_id("").has_value());
    EXPECT_FALSE(decode_client_id("02WP8qFtGJTkHk10j5u4a").has_value());   // 21
    EXPECT_FALSE(decode_client_id("02WP8qFtGJTkHk10j5u4aZZ").has_value()); // 23
    EXPECT_FALSE(decode_client_id("02WP8qFtGJTkHk10j5u4a-").has_value());  // not base62
    // 62^22 exceeds 2^128
    EXPECT_FALSE(decode_client_id("zzzzzzzzzzzzzzzzzzzzzz").has_value());

    EXPECT_FALSE(encode_client_id(*ExchangeClientOrderId::from("short")).has_value());
    EXPECT_FALSE(encode_client_id(*ExchangeClientOrderId::from("01ARZ3NDEKTSV4RRFFQ69G5FAU"))
                     .has_value()); // U is not in Crockford base32
}

namespace {

// self-computed vectors; xt's published example does not reproduce, cross-checked against the go sdk
constexpr std::string_view k_vec_secret = "bc6630d0231fda5cd98794f52c4998659beda290";
constexpr std::string_view k_vec_appkey = "3976eb88-76d0-4f6e-a6b2-a57980770085";
constexpr std::uint64_t k_vec_ts_ms = 1'641'446'237'201ULL;

// the builder backdates by 500 ms
constexpr UnixNanos k_vec_now{(k_vec_ts_ms + 500) * 1'000'000ULL};

constexpr std::string_view
    k_vec_x_spot = "validate-algorithms=HmacSHA256&validate-appkey=3976eb88-76d0-4f6e-a6b2-a57980770085"
                   "&validate-recvwindow=5000&validate-timestamp=1641446237201";
constexpr std::string_view
    k_vec_x_futures = "validate-appkey=3976eb88-76d0-4f6e-a6b2-a57980770085&validate-timestamp=1641446237201";

void load_credentials(net::Signer& signer) {
    signer.set_api_key(k_vec_appkey);
    ASSERT_TRUE(signer.secret().assign(k_vec_secret).has_value());
}

std::string_view header_of(const net::RestRequest& request, std::string_view name) {
    for (const auto& header : request.headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return {};
}

} // namespace

TEST(XtSign, BuilderReproducesTheVerifiedVectors) {
    exec::InstrumentTable table;
    load_spot_table(table);
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    const auto* spot = table.find("btc_usdt");
    ASSERT_NE(spot, nullptr);

    OrderCmd cancel{};
    cancel.instrument = base_of(*spot).id;
    cancel.target_exchange_order_id = *ExchangeOrderId::from("123");
    const auto delete_request = builder.cancel_order(cancel, table, k_vec_now);
    ASSERT_TRUE(delete_request.has_value()) << to_string(delete_request.error());
    EXPECT_EQ(builder.last_sign_message(), std::string{k_vec_x_spot} + "#DELETE#/v4/order/123");
    EXPECT_EQ(header_of(*delete_request, "validate-signature"),
              "62a0f896fa78a84769d91751e5edae294f2f709b626d09c6ef11a59ef7fc36a7");
    EXPECT_EQ(delete_request->target, "/v4/order/123");
    EXPECT_TRUE(delete_request->body.empty());

    const auto open_request = builder.open_orders(MarketType::Spot, "btc_usdt", k_vec_now);
    ASSERT_TRUE(open_request.has_value());
    EXPECT_EQ(builder.last_sign_message(),
              std::string{k_vec_x_spot} + "#GET#/v4/open-order#bizType=SPOT&symbol=btc_usdt");
    EXPECT_EQ(header_of(*open_request, "validate-signature"),
              "6f41a6c1e465d2c8ade96bd0e5d7079d67556b4c3092279c2cd0169537faf380");

    const auto key_request = builder.listen_key(k_vec_now);
    ASSERT_TRUE(key_request.has_value());
    EXPECT_EQ(builder.last_sign_message(), std::string{k_vec_x_futures} + "#/future/user/v1/user/listen-key");
    EXPECT_EQ(header_of(*key_request, "validate-signature"),
              "9784e9dfc2c62fac8904eea27724e2a3a54db0d2cdb187e2010a6c1b0f67c382");
}

// digest computed independently of the builder
TEST(XtSign, TheBalanceReadSignsAsAFuturesGetWithNoParameters) {
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    const auto request = builder.futures_balances(k_vec_now);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());
    EXPECT_EQ(request->method, net::HttpMethod::Get);
    EXPECT_EQ(request->target, "/future/user/v1/balance/list");
    EXPECT_TRUE(request->body.empty());
    // no method in Y and no trailing # for an empty query
    EXPECT_EQ(builder.last_sign_message(), std::string{k_vec_x_futures} + "#/future/user/v1/balance/list");
    EXPECT_EQ(header_of(*request, "validate-signature"),
              "866cd718030736690ac19cc293859dcfb3330747fd6291e0de7e6fece4821c02");
}

TEST(XtSign, TheVerifiedVectorsAreWhatHmacSha256Produces) {
    struct Vector {
        std::string_view message;
        std::string_view digest;
    };

    const std::array<Vector, 3> vectors{
        Vector{std::string_view{"validate-algorithms=HmacSHA256&validate-appkey=3976eb88-76d0-4f6e-a6b2-"
                                "a57980770085&validate-recvwindow=5000&validate-timestamp=1641446237201#POST#"
                                "/v4/order#{\"symbol\":\"btc_usdt\",\"side\":\"BUY\",\"type\":\"LIMIT\","
                                "\"timeInForce\":\"GTC\",\"bizType\":\"SPOT\",\"price\":39000,\"quantity\":2}"},
               "0ae356200e986654d47482b36e22a33530ba68ad5f446c94618db4f7245f7ee5"},
        Vector{std::string_view{"validate-appkey=3976eb88-76d0-4f6e-a6b2-a57980770085&validate-timestamp="
                                "1641446237201#/future/trade/v1/order/create#{\"symbol\":\"btc_usdt\","
                                "\"orderSide\":\"BUY\",\"orderType\":\"LIMIT\",\"origQty\":1,\"price\":39000,"
                                "\"positionSide\":\"LONG\",\"timeInForce\":\"GTC\"}"},
               "d38ebccc98597f30c2ad0f0533d80ef57083ca0a7f3d61719da4a7055d6b63ec"},
        Vector{std::string_view{"validate-appkey=3976eb88-76d0-4f6e-a6b2-a57980770085&validate-"
                                "timestamp=1641446237201#/future/trade/v1/order/detail#orderId=123"},
               "ff5dd36741fdcfcf6c02905d3fadfe7911d41e7d51fc5adf5b2e84716f46f9fe"},
    };
    std::array<char, net::k_hex_bytes> out{};
    for (const auto& vector : vectors) {
        EXPECT_EQ(net::hmac_sha256_hex(k_vec_secret, vector.message, out), vector.digest);
    }
}

TEST(XtSign, TheTwoTreesSignDifferentlyOnPurpose) {
    exec::InstrumentTable table;
    load_perp_table(table);
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    ASSERT_TRUE(builder.positions(MarketType::Perp, k_vec_now).has_value());
    const std::string futures{builder.last_sign_message()};
    EXPECT_TRUE(futures.starts_with(k_vec_x_futures));
    EXPECT_EQ(futures.find("validate-recvwindow"), std::string::npos);
    EXPECT_EQ(futures.find("validate-algorithms"), std::string::npos);
    EXPECT_EQ(futures.find("#GET#"), std::string::npos);
    EXPECT_EQ(futures, std::string{k_vec_x_futures} + "#/future/user/v1/position/list");

    ASSERT_TRUE(builder.my_trades(MarketType::Perp, UnixNanos{0}, k_vec_now).has_value());
    EXPECT_EQ(builder.last_sign_message(),
              std::string{k_vec_x_futures} + "#/future/trade/v1/order/trade-list#page=1&size=100");

    exec::InstrumentTable spot_table;
    load_spot_table(spot_table);
    ASSERT_TRUE(builder.my_trades(MarketType::Spot, UnixNanos{0}, k_vec_now).has_value());
    EXPECT_EQ(builder.last_sign_message(), std::string{k_vec_x_spot} + "#GET#/v4/trade#bizType=SPOT&limit=100");
}

TEST(XtSign, WithoutCredentialsNothingIsBuilt) {
    net::Signer signer;
    SignedRequestBuilder builder{signer};
    const auto request = builder.listen_key(k_vec_now);
    ASSERT_FALSE(request.has_value());
    EXPECT_EQ(request.error(), BuildError::NotSigned);
}

TEST(XtRest, FuturesOrderNamesTheContractsAndThePositionItOpens) {
    exec::InstrumentTable table;
    load_perp_table(table);
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    const auto* perp = table.find("btc_usdt");
    ASSERT_NE(perp, nullptr);
    const auto& base = base_of(*perp);

    OrderCmd cmd{};
    cmd.instrument = base.id;
    cmd.side = Side::Buy;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.price = PriceTicks::from_raw(641'181); // 64118.1 at a 0.1 tick
    cmd.qty = QtyLots::from_raw(12);           // 12 contracts at a lot of 1
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("01ARZ3NDEKTSV4RRFFQ69G5FAV");

    const auto request = builder.create_order(cmd, table, k_vec_now);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());
    EXPECT_EQ(request->method, net::HttpMethod::Post);
    EXPECT_EQ(request->target, "/future/trade/v1/order/create");
    EXPECT_EQ(request->body,
              R"({"symbol":"btc_usdt","orderSide":"BUY","orderType":"LIMIT","origQty":12,)"
              R"("price":64118.1,"timeInForce":"GTC","positionSide":"LONG",)"
              R"("clientOrderId":"02WP8qFtGJTkHk10j5u4aZ"})");
    // the whole body is signed
    EXPECT_NE(std::string{builder.last_sign_message()}.find(std::string{request->body}), std::string::npos);
}

TEST(XtRest, ReduceOnlyFlipsThePositionSideNotTheOrderSide) {
    EXPECT_EQ(position_side(Side::Buy, OrderFlags::None), "LONG");
    EXPECT_EQ(position_side(Side::Sell, OrderFlags::None), "SHORT");
    EXPECT_EQ(position_side(Side::Sell, OrderFlags::ReduceOnly), "LONG");
    EXPECT_EQ(position_side(Side::Buy, OrderFlags::ReduceOnly), "SHORT");
}

TEST(XtRest, PostOnlyIsATimeInForceHereRatherThanAFlag) {
    EXPECT_EQ(time_in_force(TimeInForce::Gtc, OrderFlags::None), "GTC");
    EXPECT_EQ(time_in_force(TimeInForce::Ioc, OrderFlags::None), "IOC");
    EXPECT_EQ(time_in_force(TimeInForce::Fok, OrderFlags::None), "FOK");
    EXPECT_EQ(time_in_force(TimeInForce::Gtc, OrderFlags::PostOnly), "GTX");
    // post-only replaces the tif
    EXPECT_EQ(time_in_force(TimeInForce::Ioc, OrderFlags::PostOnly), "GTX");
}

TEST(XtRest, AFractionalContractCountIsRefusedNotRounded) {
    exec::InstrumentTable table;
    load_perp_table(table);
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    const auto* perp = table.find("btc_usdt");
    ASSERT_NE(perp, nullptr);
    // a lot of 1 keeps every QtyLots whole, so use a finer lot to provoke the refusal
    auto forged = base_of(*perp);
    forged.lot_size = FixedE9::from_raw(k_e9 / 10);
    exec::InstrumentTable fine;
    forged.exchange_symbol = *SymbolText::from("frac_usdt");
    forged.local_symbol = forged.exchange_symbol;
    forged.id = InstrumentId::make(VenueId::Xt, MarketType::Perp, SymbolId{0});
    const auto id = fine.add(PerpInstrument{forged, 0});
    ASSERT_TRUE(id.has_value());

    OrderCmd cmd{};
    cmd.instrument = *id;
    cmd.price = PriceTicks::from_raw(641'181);
    cmd.qty = QtyLots::from_raw(15); // 1.5 contracts
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("01ARZ3NDEKTSV4RRFFQ69G5FAV");
    const auto request = builder.create_order(cmd, fine, k_vec_now);
    ASSERT_FALSE(request.has_value());
    EXPECT_EQ(request.error(), BuildError::FractionalContracts);
}

TEST(XtRest, SpotOrderCarriesBizTypeAndTheEncodedClientId) {
    exec::InstrumentTable table;
    load_spot_table(table);
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    const auto* spot = table.find("btc_usdt");
    ASSERT_NE(spot, nullptr);

    OrderCmd cmd{};
    cmd.instrument = base_of(*spot).id;
    cmd.side = Side::Sell;
    cmd.order_type = OrderType::Limit;
    cmd.price = PriceTicks::from_raw(4'000'000); // 40000 at a 0.01 tick
    cmd.qty = QtyLots::from_raw(200'000);        // 2 at a 0.00001 lot
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("01ARZ3NDEKTSV4RRFFQ69G5FAV");

    const auto request = builder.create_order(cmd, table, k_vec_now);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());
    EXPECT_EQ(request->body,
              R"({"symbol":"btc_usdt","clientOrderId":"02WP8qFtGJTkHk10j5u4aZ","side":"SELL",)"
              R"("type":"LIMIT","timeInForce":"GTC","bizType":"SPOT","price":40000,"quantity":2})");

    cmd.order_type = OrderType::Market;
    cmd.side = Side::Buy;
    const auto market_buy = builder.create_order(cmd, table, k_vec_now);
    ASSERT_FALSE(market_buy.has_value());
    EXPECT_EQ(market_buy.error(), BuildError::MarketBuyNeedsQuoteQty);

    cmd.side = Side::Sell;
    const auto market_sell = builder.create_order(cmd, table, k_vec_now);
    ASSERT_TRUE(market_sell.has_value());
    EXPECT_EQ(std::string{market_sell->body}.find("price"), std::string::npos);
}

TEST(XtRest, ACancelWithoutTheVenueOrderIdIsRefused) {
    exec::InstrumentTable table;
    load_perp_table(table);
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    const auto* perp = table.find("btc_usdt");
    ASSERT_NE(perp, nullptr);
    OrderCmd cmd{};
    cmd.instrument = base_of(*perp).id;
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("01ARZ3NDEKTSV4RRFFQ69G5FAV");
    EXPECT_EQ(builder.cancel_order(cmd, table, k_vec_now).error(), BuildError::NoOrderTarget);

    cmd.target_exchange_order_id = *ExchangeOrderId::from("538511221008857280");
    const auto request = builder.cancel_order(cmd, table, k_vec_now);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->target, "/future/trade/v1/order/cancel");
    EXPECT_EQ(request->body, R"({"orderId":538511221008857280})");
}

TEST(XtRest, AmendRestatesBothLegsOnEitherTree) {
    exec::InstrumentTable perp_table;
    load_perp_table(perp_table);
    exec::InstrumentTable spot_table;
    load_spot_table(spot_table);
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    OrderCmd cmd{};
    cmd.instrument = base_of(*perp_table.find("btc_usdt")).id;
    cmd.target_exchange_order_id = *ExchangeOrderId::from("538511221008857280");
    cmd.price = PriceTicks::from_raw(641'181);
    cmd.qty = QtyLots::from_raw(12);
    const auto perp = builder.amend_order(cmd, perp_table, k_vec_now);
    ASSERT_TRUE(perp.has_value()) << to_string(perp.error());
    EXPECT_EQ(perp->target, "/future/trade/v1/order/update");
    EXPECT_EQ(perp->body, R"({"orderId":538511221008857280,"price":64118.1,"origQty":12})");

    cmd.instrument = base_of(*spot_table.find("btc_usdt")).id;
    cmd.target_exchange_order_id = *ExchangeOrderId::from("6216559590087220004");
    cmd.price = PriceTicks::from_raw(4'000'000);
    cmd.qty = QtyLots::from_raw(200'000);
    const auto spot = builder.amend_order(cmd, spot_table, k_vec_now);
    ASSERT_TRUE(spot.has_value()) << to_string(spot.error());
    EXPECT_EQ(spot->method, net::HttpMethod::Put);
    EXPECT_EQ(spot->target, "/v4/order/6216559590087220004");
}

TEST(XtWire, FuturesOrdersArriveAsAnArrayOrAsAPage) {
    static thread_local FuturesOrderBuffer storage;
    const auto array_body = fixture("rest_futures_open_orders.json");
    const auto unpaged = parse_futures_orders(array_body, storage);
    ASSERT_TRUE(unpaged.has_value());
    ASSERT_EQ(unpaged->size(), 2U);
    EXPECT_EQ((*unpaged)[0].order_id, "538511221008857280");
    EXPECT_EQ((*unpaged)[0].price, "64118.1");
    EXPECT_EQ((*unpaged)[0].state, WireOrderStatus::New);
    EXPECT_EQ((*unpaged)[1].state, WireOrderStatus::PartiallyFilled);
    EXPECT_EQ((*unpaged)[1].executed_qty, "5");
    EXPECT_EQ((*unpaged)[1].position_side, "SHORT");

    // the paged read sends orderId, price and origQty as bare numbers
    const auto page_body = fixture("rest_futures_order_list.json");
    const auto paged = parse_futures_orders(page_body, storage);
    ASSERT_TRUE(paged.has_value());
    ASSERT_EQ(paged->size(), 1U);
    EXPECT_EQ((*paged)[0].order_id, "538511221008857280");
    EXPECT_EQ((*paged)[0].price, "64200.5");
    EXPECT_EQ((*paged)[0].orig_qty, "3");
}

TEST(XtWire, FuturesDealsCarryLiquidityInEitherSpelling) {
    static thread_local FuturesDealBuffer storage;
    const auto body = fixture("rest_futures_trade_list.json");
    const auto deals = parse_futures_deals(body, storage);
    ASSERT_TRUE(deals.has_value());
    ASSERT_EQ(deals->size(), 2U);
    EXPECT_EQ((*deals)[0].exec_id, "690000000000001");
    EXPECT_TRUE((*deals)[0].liquidity_known);
    EXPECT_TRUE((*deals)[0].maker);
    EXPECT_TRUE((*deals)[1].liquidity_known);
    EXPECT_FALSE((*deals)[1].maker);
    EXPECT_TRUE((*deals)[0].order_side.empty());

    const auto push_body = fixture("ws_futures_trade.json");
    const auto push = parse_futures_deal(push_body);
    ASSERT_TRUE(push.has_value());
    EXPECT_TRUE(push->liquidity_known);
    EXPECT_TRUE(push->maker); // isMaker, a boolean
    EXPECT_EQ(push->order_side, "BUY");
    EXPECT_EQ(push->fee, "0.0512944");
}

TEST(XtWire, TheSpotSocketSpeaksTwoLetterKeys) {
    const auto body = fixture("ws_spot_order_partial.json");
    const auto wire = parse_spot_order_push(body);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "btc_usdt");
    EXPECT_EQ(wire->order_id, "6216559590087220004");
    EXPECT_EQ(wire->client_order_id, "02WP8qFtGJTkHk10j5u4aZ");
    EXPECT_EQ(wire->state, WireOrderStatus::PartiallyFilled);
    EXPECT_EQ(wire->side, "BUY");
    EXPECT_EQ(wire->orig_qty, "4"); // `oq`, not `oqq`
    EXPECT_EQ(wire->executed_qty, "2");
    EXPECT_EQ(wire->leaving_qty, "2");
    EXPECT_EQ(wire->price, "4000");
    EXPECT_EQ(wire->avg_price, "3000");

    // i is the trade id on the fill push and the order id on the order push
    const auto trade_body = fixture("ws_spot_trade.json");
    const auto trade = parse_spot_trade_push(trade_body);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->trade_id, "6316559590087222000");
    EXPECT_EQ(trade->order_id, "6616559590087222666");
    EXPECT_TRUE(trade->liquidity_known);
    EXPECT_TRUE(trade->maker); // tm 2
    EXPECT_TRUE(trade->side.empty());
}

TEST(XtWire, OrderAcksNameTheOrderInEitherShape) {
    const auto bare = fixture("rest_futures_order_ack.json");
    const auto futures = parse_order_ack(bare);
    ASSERT_TRUE(futures.has_value());
    EXPECT_EQ(*futures, "538511221008857280");

    const auto created = fixture("rest_spot_order_ack.json");
    const auto spot = parse_order_ack(created);
    ASSERT_TRUE(spot.has_value());
    EXPECT_EQ(*spot, "6216559590087220004");

    const auto cancelled = fixture("rest_spot_cancel_ack.json");
    const auto cancel = parse_order_ack(cancelled);
    ASSERT_TRUE(cancel.has_value());
    EXPECT_EQ(*cancel, "6216559590087220004");
}

TEST(XtWire, PrivateFramesSeparateAnAckFromADeadKey) {
    const auto ack_body = fixture("ws_private_ack.json");
    const auto ack = parse_private_frame(ack_body);
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->topic, PrivateTopic::Ack);
    EXPECT_EQ(ack->code, 0);

    const auto dead_body = fixture("ws_private_listen_key_invalid.json");
    const auto dead = parse_private_frame(dead_body);
    ASSERT_TRUE(dead.has_value());
    EXPECT_EQ(dead->topic, PrivateTopic::Refusal);
    EXPECT_EQ(dead->code, k_listen_key_invalid);

    const auto order_body = fixture("ws_futures_order_new.json");
    const auto order = parse_private_frame(order_body);
    ASSERT_TRUE(order.has_value());
    EXPECT_EQ(order->topic, PrivateTopic::Order);

    const auto trade_body = fixture("ws_spot_trade.json");
    const auto trade = parse_private_frame(trade_body);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->topic, PrivateTopic::Trade);

    EXPECT_FALSE(parse_private_frame("pong").has_value());
}

TEST(XtWire, StreamKeysComeOutOfTheirOwnEnvelopes) {
    const auto key_body = fixture("rest_listen_key.json");
    const auto key = parse_listen_key(key_body);
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(*key, "A246C3DF8EE532DC75007BC5D86698541678596355681");

    const auto token_body = fixture("rest_ws_token.json");
    const auto token = parse_spot_ws_token(token_body);
    ASSERT_TRUE(token.has_value());
    EXPECT_TRUE(token->starts_with("eyJhb"));
    // the socket takes accessToken, not refreshToken
    EXPECT_GT(token->size(), 100U);
}

TEST(XtWire, TheSpotEnvelopeIsWhatSaysACallFailed) {
    const auto ok_body = fixture("rest_spot_open_orders.json");
    const auto ok = parse_spot_envelope(ok_body);
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->rc, k_spot_ok);
    EXPECT_EQ(ok->mc, k_spot_success);

    // spot refuses at http 200, so a body without rc must fail
    EXPECT_FALSE(parse_spot_envelope(R"({"mc":"SUCCESS"})").has_value());
}

TEST(XtWire, SpotServerTimeIsAnObjectUnlikeTheFuturesOne) {
    const auto spot_body = fixture("rest_spot_server_time.json");
    const auto spot = parse_spot_server_time(spot_body);
    ASSERT_TRUE(spot.has_value());
    EXPECT_GT(*spot, 1'700'000'000'000ULL);

    const auto futures_body = fixture("rest_server_time.json");
    const auto futures = parse_server_time(futures_body);
    ASSERT_TRUE(futures.has_value());
    EXPECT_GT(*futures, 1'700'000'000'000ULL);
}

TEST(XtMap, SpotCatalogueAcceptsOnlyPairsTheApiMayTrade) {
    static thread_local SpotSymbolBuffer storage;
    const auto body = fixture("rest_spot_symbol_list.json");
    const auto wire = parse_spot_symbols(body, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->size(), 6U);

    std::vector<std::string_view> accepted;
    for (const auto& row : *wire) {
        if (map_spot_instrument(row).has_value()) {
            accepted.push_back(row.symbol);
        }
    }
    // sss_usdt: tradingEnabled off, btx_usdt: openapiEnabled off, sats_usdt: precision too fine, tora_usdt: OFFLINE
    EXPECT_EQ(accepted, (std::vector<std::string_view>{"btc_usdt", "eth_usdt"}));

    const auto btc = map_spot_instrument((*wire)[0]);
    ASSERT_TRUE(btc.has_value());
    const auto& base = base_of(*btc);
    EXPECT_EQ(base.id.market(), MarketType::Spot);
    EXPECT_EQ(base.tick_size.raw, k_e9 / 100);    // pricePrecision 2
    EXPECT_EQ(base.lot_size.raw, k_e9 / 100'000); // quantityPrecision 5
    EXPECT_EQ(base.min_notional.raw, k_e9);       // QUOTE_QTY min "1"
    EXPECT_EQ(base.contract_value.raw, k_e9);     // spot quotes base units
}

TEST(XtMap, FuturesOrderPushBecomesALifecycleEvent) {
    exec::InstrumentTable table;
    load_perp_table(table);

    const auto new_body = fixture("ws_futures_order_new.json");
    const auto wire = parse_futures_order(new_body);
    ASSERT_TRUE(wire.has_value());
    EXPECT_TRUE(wire->client_order_id.empty());
    const auto remembered = *ExchangeClientOrderId::from("01ARZ3NDEKTSV4RRFFQ69G5FAV");
    const auto accepted = map_futures_order_event(*wire, table, remembered, UnixNanos{7});
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(accepted->type, OrderEventType::Accepted);
    EXPECT_EQ(accepted->side, Side::Buy);
    EXPECT_EQ(accepted->price.raw, 641'181);
    EXPECT_EQ(accepted->qty.raw, 12);
    EXPECT_EQ(accepted->leaves_qty.raw, 12);
    EXPECT_EQ(accepted->exchange_client_order_id.view(), remembered.view());
    EXPECT_EQ(accepted->exchange_order_id.view(), "538511221008857280");

    const auto cancelled_body = fixture("ws_futures_order_partially_canceled.json");
    const auto cancelled_wire = parse_futures_order(cancelled_body);
    ASSERT_TRUE(cancelled_wire.has_value());
    EXPECT_EQ(cancelled_wire->state, WireOrderStatus::Cancelled);
    const auto cancelled = map_futures_order_event(*cancelled_wire, table, remembered, UnixNanos{7});
    ASSERT_TRUE(cancelled.has_value());
    EXPECT_EQ(cancelled->type, OrderEventType::Canceled);
    EXPECT_EQ(cancelled->leaves_qty.raw, 0);
}

TEST(XtMap, AFilledOrderRecordProducesNoLifecycleEvent) {
    exec::InstrumentTable table;
    load_perp_table(table);
    static thread_local FuturesOrderBuffer storage;
    const auto body = fixture("rest_futures_open_orders.json");
    const auto orders = parse_futures_orders(body, storage);
    ASSERT_TRUE(orders.has_value());
    ASSERT_EQ(orders->size(), 2U);
    EXPECT_FALSE(map_futures_order_event((*orders)[1], table, ExchangeClientOrderId{}, UnixNanos{1}).has_value());

    const auto report = map_futures_open_order((*orders)[1], table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(report->qty.raw, 12);
    EXPECT_EQ(report->leaves.raw, 7); // 12 ordered, 5 executed
    EXPECT_EQ(report->side, Side::Sell);
    EXPECT_EQ(report->exchange_client_order_id.view(), "01ARZ3NDEKTSV4RRFFQ69G5FAV");
}

TEST(XtMap, AFillWithoutASideIsDroppedRatherThanGuessed) {
    exec::InstrumentTable table;
    load_perp_table(table);
    static thread_local FuturesDealBuffer storage;
    const auto body = fixture("rest_futures_trade_list.json");
    const auto deals = parse_futures_deals(body, storage);
    ASSERT_TRUE(deals.has_value());

    // trade-list has no side, so without a memo the fill is dropped
    EXPECT_FALSE(map_futures_deal((*deals)[0], table, OrderContext{}, UnixNanos{1}).has_value());

    const OrderContext context{.client_order_id = *ExchangeClientOrderId::from("01ARZ3NDEKTSV4RRFFQ69G5FAV"),
                               .leaves = QtyLots::from_raw(7),
                               .side = Side::Buy,
                               .side_known = true};
    const auto fill = map_futures_deal((*deals)[0], table, context, UnixNanos{1});
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->type, OrderEventType::Filled);
    EXPECT_EQ(fill->side, Side::Buy);
    EXPECT_EQ(fill->price.raw, 641'181);
    EXPECT_EQ(fill->qty.raw, 5);
    EXPECT_EQ(fill->leaves_qty.raw, 7);
    EXPECT_EQ(fill->liquidity, Liquidity::Maker);
    EXPECT_EQ(fill->fee.raw, 51'294'400);
    EXPECT_EQ(fill->exec_id.view(), "690000000000001");
    EXPECT_EQ(fill->exchange_client_order_id.view(), "01ARZ3NDEKTSV4RRFFQ69G5FAV");

    // the push states a side, which beats the memo
    const auto push_body = fixture("ws_futures_trade.json");
    const auto push = parse_futures_deal(push_body);
    ASSERT_TRUE(push.has_value());
    const auto pushed = map_futures_deal(*push, table, OrderContext{}, UnixNanos{1});
    ASSERT_TRUE(pushed.has_value());
    EXPECT_EQ(pushed->side, Side::Buy);
    EXPECT_EQ(pushed->exchange_client_order_id.view(), "01ARZ3NDEKTSV4RRFFQ69G5FAV");
}

TEST(XtMap, PositionSideIsTheOnlyThingThatSignsAPosition) {
    exec::InstrumentTable table;
    load_perp_table(table);
    static thread_local FuturesPositionBuffer storage;
    const auto body = fixture("rest_futures_positions.json");
    const auto positions = parse_futures_positions(body, storage);
    ASSERT_TRUE(positions.has_value());
    ASSERT_EQ(positions->size(), 2U);

    const auto shorted = map_futures_position((*positions)[0], table);
    ASSERT_TRUE(shorted.has_value());
    EXPECT_EQ(shorted->net_qty.raw, -12);
    EXPECT_EQ(shorted->avg_entry.raw, 641'181);

    EXPECT_FALSE(map_futures_position((*positions)[1], table).has_value());
}

TEST(XtMap, ABalanceIsReadAtTheVenuesResolutionRatherThanRefused) {
    static thread_local FuturesBalanceBuffer storage;
    const auto body = fixture("rest_futures_balance_list.json");
    const auto balances = parse_futures_balances(body, storage);
    ASSERT_TRUE(balances.has_value());
    ASSERT_EQ(balances->size(), 4U);

    // twelve fractional digits; truncation errs downward
    const auto usdt = map_futures_balance((*balances)[0]);
    ASSERT_TRUE(usdt.has_value());
    // xt sends the coin lowercase; AssetCode is uppercase across venues
    EXPECT_EQ(usdt->asset.view(), "USDT");
    EXPECT_EQ(usdt->total.raw, 2'078'572'647'931);
    EXPECT_EQ(usdt->available.raw, 2'035'822'647'931);
    EXPECT_TRUE(usdt->unrealized.is_undef());
    EXPECT_EQ(usdt->ts_venue.ns, 0ULL);

    // the same fields as bare numbers, as in xt's doc
    const auto btc = map_futures_balance((*balances)[1]);
    ASSERT_TRUE(btc.has_value());
    EXPECT_EQ(btc->asset.view(), "BTC");
    EXPECT_EQ(btc->total.raw, 36'389'400);
    EXPECT_EQ(btc->available.raw, 36'389'400);

    // a field xt did not send stays UNDEF
    const auto eth = map_futures_balance((*balances)[2]);
    ASSERT_TRUE(eth.has_value());
    EXPECT_EQ(eth->total.raw, 1'500'000'000);
    EXPECT_TRUE(eth->available.is_undef());

    EXPECT_FALSE(map_futures_balance((*balances)[3]).has_value());
}

TEST(XtMap, SpotRecordsMapThroughTheUppercaseEcho) {
    exec::InstrumentTable table;
    load_spot_table(table);
    static thread_local SpotOrderBuffer storage;
    const auto body = fixture("rest_spot_open_orders.json");
    const auto orders = parse_spot_orders(body, storage);
    ASSERT_TRUE(orders.has_value());
    ASSERT_EQ(orders->size(), 2U);
    EXPECT_EQ((*orders)[0].symbol, "BTC_USDT");

    const auto partial = map_spot_open_order((*orders)[0], table);
    ASSERT_TRUE(partial.has_value());
    EXPECT_EQ(partial->status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(partial->qty.raw, 200'000);   // 2 at a 0.00001 lot
    EXPECT_EQ(partial->leaves.raw, 80'000); // XT states `leavingQty` outright
    EXPECT_EQ(partial->price.raw, 4'000'000);
    EXPECT_EQ(partial->exchange_client_order_id.view(), "01ARZ3NDEKTSV4RRFFQ69G5FAV");

    const auto foreign = map_spot_open_order((*orders)[1], table);
    ASSERT_TRUE(foreign.has_value());
    EXPECT_TRUE(foreign->exchange_client_order_id.empty());
    EXPECT_EQ(foreign->status, OrderStatus::Accepted);
    EXPECT_EQ(foreign->side, Side::Sell);

    static thread_local SpotTradeBuffer trades_storage;
    const auto trades_body = fixture("rest_spot_trades.json");
    const auto trades = parse_spot_trades(trades_body, trades_storage);
    ASSERT_TRUE(trades.has_value());
    ASSERT_EQ(trades->size(), 1U);
    const auto fill = map_spot_trade((*trades)[0], table, {.leaves = QtyLots::from_raw(80'000)}, UnixNanos{1});
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->side, Side::Buy); // /v4/trade states `orderSide`
    EXPECT_EQ(fill->qty.raw, 120'000);
    EXPECT_EQ(fill->leaves_qty.raw, 80'000);
    EXPECT_EQ(fill->liquidity, Liquidity::Taker); // `takerMaker` is lowercase here
    EXPECT_EQ(fill->exec_id.view(), "6316559590087222001");
}

TEST(XtMap, TheSpotSocketFillTakesItsSideFromTheMemo) {
    exec::InstrumentTable table;
    load_spot_table(table);
    const auto body = fixture("ws_spot_trade.json");
    const auto wire = parse_spot_trade_push(body);
    ASSERT_TRUE(wire.has_value());
    // b is about the print's buyer, so with no memo there is no side
    EXPECT_FALSE(map_spot_trade(*wire, table, OrderContext{}, UnixNanos{1}).has_value());

    const auto fill = map_spot_trade(
        *wire, table, {.leaves = QtyLots::from_raw(179'000), .side = Side::Sell, .side_known = true}, UnixNanos{1});
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->side, Side::Sell);
    EXPECT_EQ(fill->qty.raw, 21'000); // 0.21 at a 0.00001 lot
    EXPECT_EQ(fill->liquidity, Liquidity::Maker);
}

TEST(XtMap, SpotOrderPushBecomesAnAcceptedAndThenNothing) {
    exec::InstrumentTable table;
    load_spot_table(table);

    const auto new_body = fixture("ws_spot_order_new.json");
    const auto fresh = parse_spot_order_push(new_body);
    ASSERT_TRUE(fresh.has_value());
    const auto accepted = map_spot_order_event(*fresh, table, UnixNanos{3});
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(accepted->type, OrderEventType::Accepted);
    EXPECT_EQ(accepted->qty.raw, 400'000);
    EXPECT_EQ(accepted->leaves_qty.raw, 400'000);
    EXPECT_EQ(accepted->exchange_client_order_id.view(), "01ARZ3NDEKTSV4RRFFQ69G5FAV");

    const auto partial_body = fixture("ws_spot_order_partial.json");
    const auto partial = parse_spot_order_push(partial_body);
    ASSERT_TRUE(partial.has_value());
    EXPECT_FALSE(map_spot_order_event(*partial, table, UnixNanos{3}).has_value());
}

TEST(XtMap, PrivateRecordsRefuseTheOtherMarketsInstrument) {
    exec::InstrumentTable perp_only;
    load_perp_table(perp_only);
    const auto body = fixture("ws_spot_order_partial.json");
    const auto wire = parse_spot_order_push(body);
    ASSERT_TRUE(wire.has_value());
    ASSERT_NE(perp_only.find(wire->symbol), nullptr);                // the name resolves...
    EXPECT_FALSE(map_spot_open_order(*wire, perp_only).has_value()); // ...to a perp

    exec::InstrumentTable spot_only;
    load_spot_table(spot_only);
    const auto perp_body = fixture("ws_futures_order_new.json");
    const auto perp_wire = parse_futures_order(perp_body);
    ASSERT_TRUE(perp_wire.has_value());
    EXPECT_FALSE(map_futures_open_order(*perp_wire, spot_only).has_value());
}

TEST(XtWs, TheTwoPrivateTreesSubscribeDifferently) {
    PrivateProtocol protocol;
    const auto futures = protocol.futures_subscribe("LISTENKEY123");
    ASSERT_TRUE(futures.has_value());
    // uppercase method, key appended to every topic
    EXPECT_EQ(*futures,
              R"({"method":"SUBSCRIBE","params":["order@LISTENKEY123","trade@LISTENKEY123"],)"
              R"("id":"1"})");

    const auto spot = protocol.spot_subscribe("ACCESSTOKEN123");
    ASSERT_TRUE(spot.has_value());
    // lowercase method, bare topics, key as its own field
    EXPECT_EQ(*spot,
              R"({"method":"subscribe","params":["order","trade"],)"
              R"("listenKey":"ACCESSTOKEN123","id":"2"})");

    EXPECT_FALSE(protocol.futures_subscribe({}).has_value());
    EXPECT_FALSE(protocol.spot_subscribe({}).has_value());
    EXPECT_EQ(PrivateProtocol::ping_frame(), "ping");
}

TEST(XtWs, PrivateHostsAreNotThePublicOnes) {
    EXPECT_EQ(private_stream_host(), "fstream.xt.com");
    EXPECT_EQ(private_stream_path(), "/ws/user");
    EXPECT_EQ(spot_private_stream_host(), "stream.xt.com");
    EXPECT_EQ(spot_private_stream_path(), "/private");
    EXPECT_NE(private_stream_path(), stream_path(MarketType::Perp));
}

TEST(XtMap, AFuturesTickerStatesItsBaseVolumeInContracts) {
    exec::InstrumentTable table;
    load_perp_table(table);
    const auto body = fixture("ws_ticker.json");
    const auto wire = parse_ticker(body, MarketType::Perp);
    ASSERT_TRUE(wire.has_value());
    const auto stats = map_ticker_stats(*wire, table, UnixNanos{1});
    ASSERT_TRUE(stats.has_value());
    // 166 727 460 contracts of 0.0001 BTC.
    EXPECT_EQ(stats->volume_24h_base, *parse_decimal_e9("16672.746"));
    EXPECT_EQ(stats->volume_24h_quote, *parse_turnover_e6("1403037620.60655"));

    WireTicker silent = *wire;
    silent.contracts_24h = {};
    const auto unstated = map_ticker_stats(silent, table, UnixNanos{1});
    ASSERT_TRUE(unstated.has_value());
    EXPECT_TRUE(unstated->volume_24h_base.is_undef());
}

TEST(XtSpotMap, ASpotTickerStatesBaseAndQuoteVolume) {
    exec::InstrumentTable table;
    load_spot_table(table);
    const auto body = fixture("ws_spot_ticker.json");
    const auto wire = parse_ticker(body, MarketType::Spot);
    ASSERT_TRUE(wire.has_value());
    const auto stats = map_ticker_stats(*wire, table, UnixNanos{1});
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->instrument.market(), MarketType::Spot);
    EXPECT_EQ(stats->volume_24h_base, *parse_decimal_e9("3793.32644"));
    EXPECT_EQ(stats->volume_24h_quote, *parse_turnover_e6("325479148.0257067"));
    EXPECT_EQ(stats->ts_event.ns, 1'791'299'456'348'000'000ULL);
}

InstrumentBase classified_xt(std::string_view symbol, std::string_view tags) {
    WireInstrument wire{};
    wire.symbol = symbol;
    wire.product_type = "perpetual";
    wire.tick_size = "0.01";
    wire.contract_size = "0.01";
    wire.tags = tags;
    wire.state = 0;
    wire.qty_precision = 0;
    wire.trade_switch = true;
    wire.open_api = true;
    const auto mapped = map_instrument(wire, MarketType::Perp);
    EXPECT_TRUE(mapped.has_value());
    auto base = mapped.has_value() ? base_of(*mapped) : InstrumentBase{};
    classify(VenueId::Xt, base);
    return base;
}

TEST(XtMap, TagsClassifyTheUnderlying) {
    const auto tsla = classified_xt("tslax_usdt", R"(["STOCK"])");
    EXPECT_EQ(tsla.asset_class, AssetClass::Equity);
    const auto xau = classified_xt("xau_usdt", R"(["METAL"])");
    EXPECT_EQ(xau.asset_class, AssetClass::Commodity);
    EXPECT_EQ(xau.underlying.view(), "XAU");
    const auto sp500 = classified_xt("sp500_usdt", R"(["INDEX"])");
    EXPECT_EQ(sp500.asset_class, AssetClass::Index);
    EXPECT_EQ(sp500.underlying.view(), "SPX");
    const auto spx = classified_xt("spx_usdt", "null");
    EXPECT_EQ(spx.asset_class, AssetClass::Crypto);
    EXPECT_EQ(spx.underlying.view(), "SPX");
    EXPECT_EQ(classified_xt("bond_usdt", R"(["BOND"])").asset_class, AssetClass::Unknown);
}

} // namespace
} // namespace raikiri::venues::xt
