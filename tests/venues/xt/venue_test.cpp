#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/venues/xt/map.hpp>
#include <raikiri/venues/xt/rest.hpp>
#include <raikiri/venues/xt/wire.hpp>
#include <raikiri/venues/xt/ws.hpp>
#include <raikiri/venues/xt/xt_venue.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace raikiri::venues::xt {
namespace {

// Every fixture here was captured from production XT in 2026-08; see
// fixtures/README.md for what came from where.
std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream file{path};
    EXPECT_TRUE(file.is_open()) << "missing fixture " << path;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// The mapper needs a table, and the table needs the catalogue: building it
// from the real symbol list keeps the tick and lot sizes under test honest
// instead of hand-chosen to make the arithmetic work.
//
// Filled in place, never returned by value: the table's lookup keys are views
// into its own key storage, so it must reserve before the first add and must
// not be reallocated afterwards.
void load_perp_table(exec::InstrumentTable& table) {
    static thread_local InstrumentBuffer storage;
    const auto body = fixture("rest_symbol_list.json");
    const auto wire = parse_instruments(body, storage);
    ASSERT_TRUE(wire.has_value());
    table.reserve(wire->size());
    for (const auto& entry : *wire) {
        if (const auto instrument = map_instrument(entry, MarketType::Perp);
            instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value());
        }
    }
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
    const auto adapter = exec::make_venue({.venue = VenueId::Xt, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Xt);
    EXPECT_NE(adapter->data, nullptr);
    EXPECT_NE(adapter->execution, nullptr);
    EXPECT_NE(adapter->instruments, nullptr);
}

TEST(XtVenue, OrderWithoutAnImplementationFailsLoudly) {
    // Silence would leave the order open in the store forever.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Xt, .enable_perp = true}, sink, clock);
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

TEST(XtVenue, ExecutionPullsRefuseTerminally) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Xt, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    const auto orders = adapter->execution->open_order_reports(MarketType::Perp);
    ASSERT_FALSE(orders.has_value());
    EXPECT_EQ(orders.error().cls, exec::ErrorClass::Terminal);
    EXPECT_FALSE(adapter->execution->position_reports(MarketType::Perp).has_value());
    EXPECT_FALSE(adapter->execution->fill_reports(MarketType::Perp, UnixNanos{0}).has_value());
}

// ------------------------------------------------------------ events ----

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

    // Live and deliberately unclaimed: agg_ticker cannot fill a Bbo.
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

// XT publishes no funding channel; this is the refusal it answers with, and
// the reason funding is polled over REST instead.
TEST(XtWire, FundingTopicIsRefusedByTheVenue) {
    const auto body = fixture("ws_error_unknown_topic.json");
    const auto err = parse_stream_ack(body);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, 1);
    EXPECT_NE(err->msg.find("funding_rate"), std::string_view::npos);
    EXPECT_EQ(err->id, "nofunding");
}

// A bad `method` draws the bare text "Invalid parameter" rather than JSON,
// which must be ignored rather than treated as a fatal frame. simdjson reads
// the leading word as a lazy scalar, so this surfaces as WrongType — the
// caller only checks that it failed.
TEST(XtWire, NonJsonFrameIsIgnoredNotFatal) {
    const auto event = parse_event("Invalid parameter");
    ASSERT_FALSE(event.has_value());
    EXPECT_EQ(event.error(), WireError::WrongType);
    EXPECT_FALSE(parse_event("").has_value());
}

// ------------------------------------------------------------- book ----

TEST(XtWire, DepthSnapshotParsesTopOfBook) {
    const auto body = fixture("ws_depth.json");
    LevelBuffer levels;
    const auto wire = parse_depth(body, levels);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "btc_usdt");
    EXPECT_EQ(wire->bids.size(), 5U);
    EXPECT_EQ(wire->asks.size(), 5U);
    EXPECT_GT(wire->update_id, 0U);
    EXPECT_GT(wire->ts_ms, 0U);
    // Bids descend and asks ascend, so element zero is the touch.
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
    // The continuity rule a book would apply: a delta fits when its pu equals
    // the previous message's u.
    EXPECT_EQ(next->prev_update_id, first->update_id);
    // fu is always pu + 1 and so carries nothing the pair does not.
    EXPECT_EQ(next->first_update_id, next->prev_update_id + 1);
}

// ------------------------------------------------------------ trades ----

// Getting this backwards inverts every signed-flow series built on it, so the
// direction is pinned to the live correlation that established it.
TEST(XtWire, TradeSideIsTheAggressorNotTheMaker) {
    const auto buy_body = fixture("ws_trade_buy.json");
    const auto bought = parse_trade(buy_body);
    ASSERT_TRUE(bought.has_value());
    EXPECT_TRUE(bought->aggressor_bought); // m == "BID": taker lifted the ask

    const auto sell_body = fixture("ws_trade_sell.json");
    const auto sold = parse_trade(sell_body);
    ASSERT_TRUE(sold.has_value());
    EXPECT_FALSE(sold->aggressor_bought); // m == "ASK": taker hit the bid
    EXPECT_EQ(sold->symbol, "btc_usdt");
    EXPECT_GT(sold->ts_ms, 0U);
}

TEST(XtWire, UnknownTradeSideIsRefused) {
    // Folding an unrecognised token into "sold" would bias the flow series
    // rather than reporting that the schema moved.
    const auto frame =
        R"({"topic":"trade","event":"trade@btc_usdt","data":{"s":"btc_usdt","p":"1.0","a":"2","m":"MID","t":1786394642785}})";
    const auto wire = parse_trade(frame);
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

// A truncated frame must not yield a struct full of plausible zeros.
TEST(XtWire, MalformedFrameIsAnError) {
    EXPECT_FALSE(parse_trade(R"({"topic":"trade","event":"x","data":{"s":"btc_usdt")").has_value());
    LevelBuffer levels;
    EXPECT_FALSE(parse_depth(R"({"topic":"depth","data":{"s":"btc_usdt"}})", levels).has_value());
}

// ------------------------------------------------------------- rest ----

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
    // Sanity, not exactness: epoch ms for a date after 2020.
    EXPECT_GT(*ms, 1'577'836'800'000ULL);
}

TEST(XtRest, FundingRateReadsANumberField) {
    const auto body = fixture("rest_funding_rate.json");
    const auto wire = parse_funding_rate(body);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "btc_usdt");
    EXPECT_EQ(wire->rate_token, "0.00008866");
    EXPECT_EQ(wire->interval_h, 8); // collectionInternal counts HOURS
    EXPECT_GT(wire->next_collection_ms, 0U);
}

TEST(XtRest, RequestTargetsCarryTheSymbolVerbatim) {
    RequestBuilder builder;
    EXPECT_EQ(builder.server_time().target, "/future/market/v1/public/time");
    EXPECT_EQ(builder.symbol_list().target, "/future/market/v1/public/symbol/list");
    EXPECT_EQ(builder.funding_rate("btc_usdt").target,
              "/future/market/v1/public/q/funding-rate?symbol=btc_usdt");
    EXPECT_EQ(builder.depth("eth_usdt", 5).target,
              "/future/market/v1/public/q/depth?symbol=eth_usdt&level=5");
}

// ------------------------------------------------------------ errors ----

TEST(XtErrors, VerifiedCodesClassifyAndUnknownIsTerminal) {
    EXPECT_EQ(classify(k_invalid_symbol), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_invalid_param), exec::ErrorClass::Terminal);
    // "400" covers both a missing appKey and a malformed request, so it must
    // not halt a venue whose credentials are fine.
    EXPECT_EQ(classify("400"), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify("something_xt_added_later"), exec::ErrorClass::Terminal);

    EXPECT_EQ(classify("401"), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify("429"), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify("503"), exec::ErrorClass::Retryable);
}

TEST(XtErrors, TextCodesSurviveAsZeroRatherThanGuessing) {
    EXPECT_EQ(numeric_code("400"), 400);
    EXPECT_EQ(numeric_code(k_invalid_symbol), 0);
    EXPECT_EQ(numeric_code(""), 0);
}

// ---------------------------------------------------------------- ws ----

TEST(XtWs, TopicsMatchWhatTheVenueAccepts) {
    // depth, not agg_ticker: agg_ticker publishes no sizes.
    EXPECT_EQ(topic_for(exec::SubKind::Bbo).channel, "depth");
    EXPECT_EQ(topic_for(exec::SubKind::Bbo).params, ",5,100ms");
    EXPECT_EQ(topic_for(exec::SubKind::Trades).channel, "trade");
    EXPECT_EQ(topic_for(exec::SubKind::Depth).channel, "depth_update");
    EXPECT_EQ(topic_for(exec::SubKind::MarkPrice).channel, "mark_price");
    // No funding topic exists on this venue at all.
    EXPECT_TRUE(topic_for(exec::SubKind::Funding).channel.empty());
    EXPECT_TRUE(topic_for(exec::SubKind::Kline).channel.empty());
}

TEST(XtWs, SubscribeFrameJoinsTopicWithAtSign) {
    Protocol protocol;
    const std::array<Subscription, 2> subs{
        Subscription{.channel = "depth", .symbol = "btc_usdt", .params = ",5,100ms"},
        Subscription{.channel = "trade", .symbol = "eth_usdt", .params = {}}};
    const auto frame = protocol.subscribe_frame(subs);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->consumed, 2U);
    EXPECT_EQ(
        frame->text,
        R"({"method":"subscribe","params":["depth@btc_usdt,5,100ms","trade@eth_usdt"],"id":"1"})");

    // The id advances per frame, which is all the correlation XT offers.
    const auto next = protocol.unsubscribe_frame(std::span{subs}.first(1));
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->text,
              R"({"method":"unsubscribe","params":["depth@btc_usdt,5,100ms"],"id":"2"})");
}

TEST(XtWs, DeclinedKindNeverReachesTheWire) {
    Protocol protocol;
    const std::array<Subscription, 1> empty_channel{
        Subscription{.channel = {}, .symbol = "btc_usdt", .params = {}}};
    const auto frame = protocol.subscribe_frame(empty_channel);
    ASSERT_FALSE(frame.has_value());
    EXPECT_EQ(frame.error(), WsBuildError::UnsupportedKind);
    EXPECT_FALSE(protocol.subscribe_frame({}).has_value());
}

// --------------------------------------------------------- catalogue ----

TEST(XtMap, StepFromPrecisionIsAnIntegerPowerOfTen) {
    EXPECT_EQ(step_from_precision(0)->raw, k_e9);       // 1
    EXPECT_EQ(step_from_precision(2)->raw, 10'000'000); // 0.01
    EXPECT_EQ(step_from_precision(9)->raw, 1);          // 1e-9
    // Finer than the domain models: refused rather than rounded to zero.
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
    // ftt_usdt has tradeSwitch=false, dia_usdt has isOpenApi=false, and
    // btc_usdt_261225 passes every switch but is a dated future.
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
    // The catalogue does not state the funding interval; the funding read does.
    EXPECT_EQ(std::get<PerpInstrument>(*instrument).funding_interval_s, 0U);
}

// XT's futures catalogue has no spot rows; answering an empty list would read
// as "XT lists no spot pairs", which is false.
TEST(XtMap, SpotIsRefusedRatherThanAnsweredEmpty) {
    static thread_local InstrumentBuffer storage;
    const auto body = fixture("rest_symbol_list.json");
    const auto wire = parse_instruments(body, storage);
    ASSERT_TRUE(wire.has_value());
    for (const auto& entry : *wire) {
        EXPECT_FALSE(map_instrument(entry, MarketType::Spot).has_value());
    }
}

// ------------------------------------------------------------ mapping ----

TEST(XtMap, BboComesFromTheTopOfTheDepthSnapshot) {
    exec::InstrumentTable table;
    load_perp_table(table);
    const auto body = fixture("ws_depth.json");
    LevelBuffer levels;
    const auto wire = parse_depth(body, levels);
    ASSERT_TRUE(wire.has_value());

    const UnixNanos received{123'456'789ULL};
    const auto bbo = map_bbo(*wire, table, received);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_LT(bbo->bid.raw, bbo->ask.raw);
    EXPECT_GT(bbo->bid_qty.raw, 0);
    EXPECT_GT(bbo->ask_qty.raw, 0);
    EXPECT_EQ(bbo->ts_init, received);
    EXPECT_NE(bbo->ts_event, received); // the venue stated its own publish time
    // Prices are exact multiples of the 0.1 tick: 64152.1 -> 641521 ticks.
    EXPECT_EQ(bbo->bid.raw * 100'000'000, parse_decimal_e9(wire->bids[0].price)->raw);
}

TEST(XtMap, UnknownSymbolAndEmptySideAreDropped) {
    exec::InstrumentTable table;
    load_perp_table(table);
    const auto body = fixture("ws_depth.json");
    LevelBuffer levels;
    auto wire = parse_depth(body, levels);
    ASSERT_TRUE(wire.has_value());

    auto foreign = *wire;
    foreign.symbol = "not_listed";
    EXPECT_FALSE(map_bbo(foreign, table, UnixNanos{1}).has_value());

    // A one-sided book is a real state on an illiquid contract, not a quote
    // at price zero.
    auto empty_asks = *wire;
    empty_asks.asks = {};
    EXPECT_FALSE(map_bbo(empty_asks, table, UnixNanos{1}).has_value());
}

TEST(XtMap, TradeMapsToAggressorSide) {
    exec::InstrumentTable table;
    load_perp_table(table);
    const auto buy_body = fixture("ws_trade_buy.json");
    const auto bought = parse_trade(buy_body);
    ASSERT_TRUE(bought.has_value());
    const auto buy = map_trade(*bought, table, UnixNanos{7});
    ASSERT_TRUE(buy.has_value());
    EXPECT_EQ(buy->aggressor, Side::Buy);
    EXPECT_GT(buy->qty.raw, 0); // a count of contracts, lot size 1

    const auto sell_body = fixture("ws_trade_sell.json");
    const auto sold = parse_trade(sell_body);
    ASSERT_TRUE(sold.has_value());
    const auto sell = map_trade(*sold, table, UnixNanos{7});
    ASSERT_TRUE(sell.has_value());
    EXPECT_EQ(sell->aggressor, Side::Sell);
}

// A price off the instrument's tick is a schema event, so the print is dropped
// rather than rounded onto a neighbouring tick.
TEST(XtMap, PriceOffTheTickIsDroppedNotRounded) {
    exec::InstrumentTable table;
    load_perp_table(table);
    const auto body = fixture("ws_trade_buy.json");
    auto wire = parse_trade(body);
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
    const auto funding = map_funding(*wire, table, received);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 88'660); // 0.00008866 in 1e9 fixed point
    EXPECT_EQ(funding->interval_s, 8U * 3600U);
    EXPECT_GT(funding->next_funding_time.ns, 0ULL);
    // XT states no publish time on this endpoint, and mark arrives elsewhere.
    EXPECT_EQ(funding->ts_event, received);
    EXPECT_TRUE(funding->mark_price.is_undef());
    EXPECT_TRUE(funding->predicted_rate.is_undef());
}

TEST(XtMap, NegativeAndPaddedFundingRatesParse) {
    exec::InstrumentTable table;
    load_perp_table(table);
    const auto body = fixture("rest_funding_rate.json");
    auto wire = parse_funding_rate(body);
    ASSERT_TRUE(wire.has_value());
    // XT pads to a fixed scale: 18 fractional digits, trailing zeros.
    auto padded = *wire;
    padded.rate_token = "-0.000002000000000000";
    const auto funding = map_funding(padded, table, UnixNanos{1});
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, -2'000);

    // Finer than 1e-9 cannot be represented; reporting zero would claim the
    // contract pays no funding.
    auto too_fine = *wire;
    too_fine.rate_token = "0.0000000000123";
    EXPECT_FALSE(map_funding(too_fine, table, UnixNanos{1}).has_value());
}

TEST(XtMap, VenueTimestampBeatsIngestTimeButZeroDoesNot) {
    const UnixNanos fallback{42ULL};
    EXPECT_EQ(ms_to_unix(0, fallback), fallback);
    EXPECT_EQ(ms_to_unix(1'786'394'629'290ULL, fallback).ns, 1'786'394'629'290'000'000ULL);
}

} // namespace
} // namespace raikiri::venues::xt
