#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/venues/kucoin/kucoin_venue.hpp>
#include <raikiri/venues/kucoin/map.hpp>
#include <raikiri/venues/kucoin/rest.hpp>
#include <raikiri/venues/kucoin/wire.hpp>
#include <raikiri/venues/kucoin/ws.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace raikiri::venues::kucoin {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path, std::ios::binary};
    EXPECT_TRUE(in.is_open()) << path;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    auto text = buffer.str();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

// The instrument table every mapping test resolves symbols against, loaded
// through the same path production uses.
exec::InstrumentTable loaded_table() {
    const auto body = fixture("rest_contracts_active.json");
    static ContractBuffer storage;
    const auto wire = parse_contracts(body, storage);
    EXPECT_TRUE(wire.has_value());
    exec::InstrumentTable table;
    for (const auto& entry : *wire) {
        if (auto instrument = map_instrument(entry, MarketType::Perp); instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value());
        }
    }
    return table;
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

TEST(KucoinVenue, RegistersAndBuilds) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto adapter =
        exec::make_venue({.venue = VenueId::Kucoin, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Kucoin);
    EXPECT_NE(adapter->data, nullptr);
    EXPECT_NE(adapter->execution, nullptr);
    EXPECT_NE(adapter->instruments, nullptr);
}

TEST(KucoinVenue, OrderWithoutAnImplementationFailsLoudly) {
    // Silence would leave the order open in the store forever.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Kucoin, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.instrument = InstrumentId::make(VenueId::Kucoin, MarketType::Perp, SymbolId{0});
    cmd.client_order_id = ClientOrderId::make(1, 42);
    adapter->execution->submit(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);

    cmd.kind = CmdKind::Cancel;
    adapter->execution->cancel(cmd);
    ASSERT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(sink.events[1].type, OrderEventType::CancelRejected);
}

TEST(KucoinVenue, CatalogueRefusesMarketsTheFuturesHostDoesNotServe) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Kucoin, .enable_spot = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    // An empty list would read as "KuCoin lists no spot pairs", which is a
    // very different claim from "this host has no spot catalogue". No network
    // is touched: the market is refused before any request is built.
    const auto spot = adapter->instruments->fetch_all(MarketType::Spot);
    ASSERT_FALSE(spot.has_value());
    EXPECT_EQ(spot.error().cls, exec::ErrorClass::Terminal);
}

// ------------------------------------------------------------- wire ----

TEST(KucoinWire, SubjectDiscriminatesEveryFrameShape) {
    struct Case {
        std::string_view file;
        EventKind kind;
        std::string_view symbol;
    };

    const std::array<Case, 9> cases{Case{"ws_welcome.json", EventKind::Welcome, ""},
                                    {"ws_ack.json", EventKind::Ack, ""},
                                    {"ws_pong.json", EventKind::Pong, ""},
                                    {"ws_error_topic.json", EventKind::Error, ""},
                                    {"ws_ticker_v2.json", EventKind::Bbo, "XBTUSDTM"},
                                    {"ws_execution.json", EventKind::Trade, "XBTUSDTM"},
                                    {"ws_level2.json", EventKind::Depth, "XBTUSDTM"},
                                    {"ws_mark_index_price.json", EventKind::MarkPrice, "XBTUSDTM"},
                                    // Same topic as mark price; only `subject` tells them apart.
                                    {"ws_funding_rate.json", EventKind::FundingRate, "XBTUSDTM"}};
    for (const auto& entry : cases) {
        const auto frame = fixture(entry.file);
        const auto event = parse_event(frame);
        ASSERT_TRUE(event.has_value()) << entry.file;
        EXPECT_EQ(event->kind, entry.kind) << entry.file;
        EXPECT_EQ(event->symbol, entry.symbol) << entry.file;
    }
}

TEST(KucoinWire, BboReadsQuotedPricesAndBareSizesFromOneObject) {
    const auto frame = fixture("ws_ticker_v2.json");
    const auto wire = parse_bbo(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "XBTUSDTM");
    EXPECT_EQ(wire->bid_price, "64125.6");
    EXPECT_EQ(wire->ask_price, "64125.7");
    // Quoted price, bare size, same object: the DTO must not care which.
    EXPECT_EQ(wire->bid_qty, "2589");
    EXPECT_EQ(wire->ask_qty, "183");
    EXPECT_EQ(wire->sequence, 1746462177152);
    // Nanoseconds, not milliseconds.
    EXPECT_EQ(wire->ts_ns, 1786394569634000000ULL);
}

TEST(KucoinWire, ParsedViewsPointIntoTheCallerBuffer) {
    // The zero-copy promise: an unescape buffer inside simdjson cannot be
    // rebased, so a view landing outside the frame is a lifetime bug.
    const auto frame = fixture("ws_ticker_v2.json");
    const auto wire = parse_bbo(frame);
    ASSERT_TRUE(wire.has_value());
    const auto* begin = frame.data();
    const auto* end = begin + frame.size();
    for (const auto view : {wire->symbol, wire->bid_price, wire->ask_price, wire->bid_qty}) {
        EXPECT_GE(view.data(), begin);
        EXPECT_LE(view.data() + view.size(), end);
    }
}

TEST(KucoinWire, TradeCarriesTheTakerSide) {
    const auto frame = fixture("ws_execution.json");
    const auto wire = parse_trade(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "XBTUSDTM");
    EXPECT_EQ(wire->trade_id, "1940359085512");
    EXPECT_EQ(wire->price, "64140");
    EXPECT_EQ(wire->qty, "1");
    EXPECT_TRUE(wire->aggressor_bought);
    EXPECT_EQ(wire->ts_ns, 1786394609912000000ULL);
}

TEST(KucoinWire, Level2PacksOneLevelIntoACommaJoinedString) {
    const auto frame = fixture("ws_level2.json");
    const auto wire = parse_book_change(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "XBTUSDTM"); // data carries no symbol; the topic does
    EXPECT_EQ(wire->price, "62234.6");
    EXPECT_EQ(wire->qty, "1");
    EXPECT_TRUE(wire->bid);
    EXPECT_EQ(wire->sequence, 1746462195520);
    // Milliseconds here, unlike the nanoseconds tickerV2 and execution send.
    EXPECT_EQ(wire->ts_ms, 1786394623641ULL);
}

TEST(KucoinWire, MalformedFramesFailInsteadOfFillingPartially) {
    EXPECT_FALSE(parse_event("not json at all").has_value());
    EXPECT_FALSE(parse_event(R"({"type":"message","subject":"tickerV2"})").has_value());
    // A change string short of three fields would apply a real price at an
    // invented size.
    EXPECT_FALSE(parse_book_change(
                     R"({"topic":"/contractMarket/level2:X","type":"message","subject":"level2",)"
                     R"("data":{"sequence":1,"change":"62234.6,buy","timestamp":1}})")
                     .has_value());
    // No symbol behind the topic's colon.
    EXPECT_FALSE(parse_bbo(R"({"topic":"/contractMarket/tickerV2","type":"message",)"
                           R"("subject":"tickerV2","data":{}})")
                     .has_value());
}

TEST(KucoinWire, StreamErrorReadsNumericCodeAndTextFromData) {
    const auto topic_frame = fixture("ws_error_topic.json");
    const auto topic = parse_stream_error(topic_frame);
    ASSERT_TRUE(topic.has_value());
    EXPECT_EQ(topic->code, k_ws_topic_not_found);
    EXPECT_EQ(topic->msg, "topic does not exist");

    const auto token_frame = fixture("ws_error_token.json");
    const auto token = parse_stream_error(token_frame);
    ASSERT_TRUE(token.has_value());
    EXPECT_EQ(token->code, k_ws_token_invalid);
    EXPECT_TRUE(is_expired_token(token->code));
}

TEST(KucoinWire, MarkPriceKeepsNumberTokensVerbatim) {
    const auto frame = fixture("ws_mark_index_price.json");
    const auto wire = parse_mark_price(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->mark_price, "64175.21");
    EXPECT_EQ(wire->index_price, "64196.45");
    EXPECT_EQ(wire->ts_ms, 1786394632000ULL);
    // Mark is quoted on indexPriceTickSize (0.01), not the contract's 0.1
    // tick, so it does not divide by the instrument's price step at all —
    // which is why FundingUpdate::mark_price is left undefined.
    const auto table = loaded_table();
    const auto* instrument = table.find(std::string_view{"XBTUSDTM"});
    ASSERT_NE(instrument, nullptr);
    const auto mark = parse_number_e9(wire->mark_price);
    ASSERT_TRUE(mark.has_value());
    EXPECT_NE(mark->raw % base_of(*instrument).tick_size.raw, 0);
}

// ------------------------------------------------------------- rest ----

TEST(KucoinRest, BulletHandshakeNamesTheEndpointAndItsKeepalive) {
    const auto body = fixture("rest_bullet_public.json");
    const auto wire = parse_bullet(body);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->endpoint, "wss://ws-api-futures.kucoin.com/");
    EXPECT_FALSE(wire->token.empty());
    EXPECT_EQ(wire->ping_interval_ms, 18000ULL);
    EXPECT_EQ(wire->ping_timeout_ms, 10000ULL);
}

TEST(KucoinRest, EnvelopeReadsTheStringCode) {
    const auto ok = parse_rest_envelope(fixture("rest_timestamp.json"));
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->code, k_ok);

    const auto missing = parse_rest_envelope(fixture("rest_error_unknown_symbol.json"));
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(missing->code, k_url_not_found);
    EXPECT_FALSE(missing->msg.empty());

    const auto unsupported = parse_rest_envelope(fixture("rest_error_unsupported.json"));
    ASSERT_TRUE(unsupported.has_value());
    EXPECT_EQ(unsupported->code, k_unsupported);

    // A code that cannot be read must never decay into success.
    EXPECT_FALSE(parse_rest_envelope(R"({"code":"oops","data":1})").has_value());
    EXPECT_FALSE(parse_rest_envelope(R"({"data":1})").has_value());
}

TEST(KucoinRest, ServerTimeReadsABareNumberPayload) {
    const auto ts = parse_server_time(fixture("rest_timestamp.json"));
    ASSERT_TRUE(ts.has_value());
    EXPECT_EQ(*ts, 1786394604446ULL);
}

TEST(KucoinRest, ContractsKeepJavaFormattedNumbersIntact) {
    static ContractBuffer storage;
    const auto body = fixture("rest_contracts_active.json");
    const auto wire = parse_contracts(body, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->size(), 4U);
    EXPECT_EQ((*wire)[0].symbol, "XBTUSDTM");
    EXPECT_EQ((*wire)[0].status, "Open");
    EXPECT_EQ((*wire)[0].type, "FFWCSX");
    EXPECT_EQ((*wire)[0].tick_size, "0.1");
    EXPECT_EQ((*wire)[0].multiplier, "0.001");
    EXPECT_EQ((*wire)[0].funding_granularity_ms, 28800000ULL);
    // Java's double formatting reaches the wire verbatim.
    EXPECT_EQ((*wire)[3].symbol, "PEPEUSDTM");
    EXPECT_EQ((*wire)[3].tick_size, "1.0E-10");
    EXPECT_EQ((*wire)[2].multiplier, "-1.0");
}

TEST(KucoinRest, FundingCurrentIgnoresTheIndexSymbolItNames) {
    const auto body = fixture("rest_funding_rate_current.json");
    const auto wire = parse_funding_current(body);
    ASSERT_TRUE(wire.has_value());
    EXPECT_TRUE(wire->symbol.empty()); // the body names ".XBTUSDTMFPI8H", not the contract
    EXPECT_EQ(wire->rate, "1.0E-4");
    EXPECT_EQ(wire->granularity_ms, 28800000ULL);
    EXPECT_EQ(wire->next_funding_ms, 1786406400000ULL);
    EXPECT_EQ(wire->ts_ms, 1786377600000ULL);
}

TEST(KucoinRest, RequestBuildersNameTheDocumentedPaths) {
    RequestBuilder builder;
    const auto bullet = builder.bullet_public();
    EXPECT_EQ(bullet.method, net::HttpMethod::Post);
    EXPECT_EQ(bullet.target, "/api/v1/bullet-public");
    EXPECT_TRUE(bullet.body.empty());
    EXPECT_EQ(builder.contracts_active().target, "/api/v1/contracts/active");
    EXPECT_EQ(builder.server_time().target, "/api/v1/timestamp");
    EXPECT_EQ(builder.funding_rate_current("XBTUSDTM").target,
              "/api/v1/funding-rate/XBTUSDTM/current");
    EXPECT_EQ(builder.level2_snapshot("XBTUSDTM").target,
              "/api/v1/level2/snapshot?symbol=XBTUSDTM");
}

// -------------------------------------------------------------- map ----

TEST(KucoinMap, NumberParserHandlesJavaExponentsWithoutRounding) {
    EXPECT_EQ(parse_number_e9("0.1")->raw, 100'000'000);
    EXPECT_EQ(parse_number_e9("1")->raw, k_e9);
    EXPECT_EQ(parse_number_e9("-1.0")->raw, -k_e9);
    EXPECT_EQ(parse_number_e9("1.0E-4")->raw, 100'000);
    EXPECT_EQ(parse_number_e9("3.0E-4")->raw, 300'000);
    EXPECT_EQ(parse_number_e9("1.0E-9")->raw, 1);
    EXPECT_EQ(parse_number_e9("520000.0")->raw, 520'000 * k_e9);
    // One decade below what FixedE9 models: refused rather than rounded to
    // zero, which would make the instrument's tick size unusable.
    EXPECT_FALSE(parse_number_e9("1.0E-10").has_value());
    EXPECT_FALSE(parse_number_e9("1.0E+40").has_value());
    EXPECT_FALSE(parse_number_e9("1.0E").has_value());
    EXPECT_FALSE(parse_number_e9("").has_value());
}

TEST(KucoinMap, CatalogueAdmitsOnlyOpenNonInversePerpetuals) {
    static ContractBuffer storage;
    const auto body = fixture("rest_contracts_active.json");
    const auto wire = parse_contracts(body, storage);
    ASSERT_TRUE(wire.has_value());

    const auto xbt = map_instrument((*wire)[0], MarketType::Perp);
    ASSERT_TRUE(xbt.has_value());
    const auto& base = base_of(*xbt);
    EXPECT_EQ(base.exchange_symbol.view(), "XBTUSDTM");
    EXPECT_EQ(base.tick_size.raw, 100'000'000);
    EXPECT_EQ(base.lot_size.raw, k_e9);
    // Sizes are CONTRACTS: one XBTUSDTM contract is 0.001 XBT, so a pinned 1
    // would misprice every notional by 1000x.
    EXPECT_EQ(base.contract_value.raw, 1'000'000);
    ASSERT_TRUE(std::holds_alternative<PerpInstrument>(*xbt));
    // The catalogue states the interval, so it is read rather than guessed.
    EXPECT_EQ(std::get<PerpInstrument>(*xbt).funding_interval_s, 28800U);

    EXPECT_TRUE(map_instrument((*wire)[1], MarketType::Perp).has_value()); // ETHUSDTM
    // FFICSX is a dated future sharing this endpoint, and its multiplier is
    // negative because it is also inverse.
    EXPECT_FALSE(map_instrument((*wire)[2], MarketType::Perp).has_value());
    // Tick below FixedE9 resolution: refused, never rounded into the table.
    EXPECT_FALSE(map_instrument((*wire)[3], MarketType::Perp).has_value());
    // The futures host serves no spot, whatever the row says.
    EXPECT_FALSE(map_instrument((*wire)[0], MarketType::Spot).has_value());
}

TEST(KucoinMap, NonOpenContractIsRefused) {
    static ContractBuffer storage;
    const auto body = fixture("rest_contracts_paused.json");
    const auto wire = parse_contracts(body, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->size(), 1U);
    EXPECT_EQ((*wire)[0].status, "Pause");
    EXPECT_FALSE(map_instrument((*wire)[0], MarketType::Perp).has_value());
}

TEST(KucoinMap, BboScalesByTheInstrumentStepsAndKeepsNanoseconds) {
    const auto table = loaded_table();
    const auto frame = fixture("ws_ticker_v2.json");
    const auto wire = parse_bbo(frame);
    ASSERT_TRUE(wire.has_value());
    const auto bbo = map_bbo(*wire, table, UnixNanos{7});
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 641'256); // 64125.6 / 0.1
    EXPECT_EQ(bbo->ask.raw, 641'257);
    EXPECT_EQ(bbo->bid_qty.raw, 2589); // contracts, lot size 1
    EXPECT_EQ(bbo->ask_qty.raw, 183);
    EXPECT_EQ(bbo->ts_event.ns, 1786394569634000000ULL);
    EXPECT_EQ(bbo->ts_init.ns, 7U);
}

TEST(KucoinMap, UnknownSymbolAndNonMultiplePricesAreDropped) {
    const auto table = loaded_table();
    WireBbo wire{};
    wire.symbol = "NOSUCHSYM";
    wire.bid_price = "1";
    wire.ask_price = "1";
    wire.bid_qty = "1";
    wire.ask_qty = "1";
    EXPECT_FALSE(map_bbo(wire, table, UnixNanos{1}).has_value());

    // 0.05 is half a tick on XBTUSDTM. Rounding it is how a quote lands one
    // tick from where the venue put it, so the message is dropped instead.
    wire.symbol = "XBTUSDTM";
    wire.bid_price = "64125.65";
    EXPECT_FALSE(map_bbo(wire, table, UnixNanos{1}).has_value());
}

TEST(KucoinMap, TradeKeepsTheAggressorDirectionAsSent) {
    const auto table = loaded_table();
    const auto frame = fixture("ws_execution.json");
    auto wire = parse_trade(frame);
    ASSERT_TRUE(wire.has_value());
    const auto buy = map_trade(*wire, table, UnixNanos{3});
    ASSERT_TRUE(buy.has_value());
    EXPECT_EQ(buy->price.raw, 641'400);
    EXPECT_EQ(buy->qty.raw, 1);
    // KuCoin states the taker's direction, so nothing is inverted; binance's
    // `m` says the opposite thing and does need a flip.
    EXPECT_EQ(buy->aggressor, Side::Buy);
    EXPECT_EQ(buy->ts_event.ns, 1786394609912000000ULL);

    wire->aggressor_bought = false;
    const auto sell = map_trade(*wire, table, UnixNanos{3});
    ASSERT_TRUE(sell.has_value());
    EXPECT_EQ(sell->aggressor, Side::Sell);
}

TEST(KucoinMap, FundingFromRestCarriesTheSettlementTheStreamOmits) {
    const auto table = loaded_table();
    const auto body = fixture("rest_funding_rate_current.json");
    const auto wire = parse_funding_current(body);
    ASSERT_TRUE(wire.has_value());
    const auto funding = map_funding(*wire, "XBTUSDTM", table, UnixNanos{11});
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 100'000); // 1.0E-4
    EXPECT_EQ(funding->interval_s, 28800U);
    EXPECT_EQ(funding->next_funding_time.ns, 1786406400000ULL * 1'000'000ULL);
    EXPECT_EQ(funding->ts_event.ns, 1786377600000ULL * 1'000'000ULL);
    // mark_price stays undefined: a zero would read as a real price of zero.
    EXPECT_TRUE(funding->mark_price.is_undef());
    // The symbol argument wins, because the body names the funding index.
    EXPECT_FALSE(map_funding(*wire, "NOSUCHSYM", table, UnixNanos{11}).has_value());
}

TEST(KucoinMap, FundingFromTheStreamIgnoresItsOwnGranularity) {
    const auto table = loaded_table();
    const auto frame = fixture("ws_funding_rate.json");
    auto wire = parse_funding_rate(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->rate, "0.000100");
    // The stream republishes once a minute and reports that cadence here,
    // where the REST body reports the 28800000 settlement period instead.
    EXPECT_EQ(wire->granularity_ms, 60'000ULL);
    EXPECT_EQ(wire->next_funding_ms, 0ULL);

    const auto funding = map_funding(*wire, wire->symbol, table, UnixNanos{5});
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 100'000);
    // Taken from the catalogue: trusting the frame would report an 8 h
    // contract as funding every minute.
    EXPECT_EQ(funding->interval_s, 28800U);
    // Settlements land on wall-clock multiples of the interval, which is what
    // the REST body's own fundingTime says for this same session.
    EXPECT_EQ(funding->next_funding_time.ns, 1786406400000ULL * 1'000'000ULL);

    // A contract that quotes no funding must not be reported as one funding
    // at zero.
    wire->rate = {};
    EXPECT_FALSE(map_funding(*wire, wire->symbol, table, UnixNanos{5}).has_value());
}

// --------------------------------------------------------------- ws ----

TEST(KucoinWs, TopicsCoverTheKindsKucoinStreams) {
    EXPECT_EQ(topic_for(exec::SubKind::Bbo), "/contractMarket/tickerV2");
    EXPECT_EQ(topic_for(exec::SubKind::Trades), "/contractMarket/execution");
    EXPECT_EQ(topic_for(exec::SubKind::Depth), "/contractMarket/level2");
    // Mark and funding share one topic; `subject` separates them.
    EXPECT_EQ(topic_for(exec::SubKind::Funding), "/contract/instrument");
    EXPECT_EQ(topic_for(exec::SubKind::MarkPrice), "/contract/instrument");
    EXPECT_TRUE(topic_for(exec::SubKind::Kline).empty());
}

TEST(KucoinWs, SubscribeBatchesOneTopicPerFrame) {
    Protocol protocol;
    const std::array<Subscription, 3> subs{
        Subscription{.topic = "/contractMarket/tickerV2", .symbol = "XBTUSDTM"},
        Subscription{.topic = "/contractMarket/tickerV2", .symbol = "ETHUSDTM"},
        Subscription{.topic = "/contractMarket/execution", .symbol = "XBTUSDTM"}};
    const auto first = protocol.subscribe_frame(subs);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->consumed, 2U); // the run sharing a topic, not the whole span
    EXPECT_EQ(first->text,
              R"({"id":1,"type":"subscribe","topic":"/contractMarket/tickerV2:XBTUSDTM,ETHUSDTM",)"
              R"("response":true})");

    const auto second = protocol.subscribe_frame(std::span{subs}.subspan(2));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->consumed, 1U);
    EXPECT_EQ(second->text,
              R"({"id":2,"type":"subscribe","topic":"/contractMarket/execution:XBTUSDTM",)"
              R"("response":true})");

    const auto unsub = protocol.unsubscribe_frame(subs);
    ASSERT_TRUE(unsub.has_value());
    EXPECT_TRUE(unsub->text.starts_with(R"({"id":3,"type":"unsubscribe")"));

    EXPECT_FALSE(protocol.subscribe_frame({}).has_value());
    const std::array<Subscription, 1> declined{Subscription{.topic = {}, .symbol = "XBTUSDTM"}};
    EXPECT_FALSE(protocol.subscribe_frame(declined).has_value());
}

TEST(KucoinWs, EveryMessageCarriesAFreshId) {
    // KuCoin echoes the id in the pong, which is the only way to tell which
    // ping was answered; a constant id would make them indistinguishable.
    Protocol protocol;
    const std::string first{protocol.ping_frame()};
    const std::string second{protocol.ping_frame()};
    EXPECT_EQ(first, R"({"id":1,"type":"ping"})");
    EXPECT_EQ(second, R"({"id":2,"type":"ping"})");
}

TEST(KucoinWs, EndpointIsBuiltFromTheBulletAnswer) {
    const auto body = fixture("rest_bullet_public.json");
    const auto bullet = parse_bullet(body);
    ASSERT_TRUE(bullet.has_value());
    const auto endpoint = make_endpoint(bullet->endpoint, bullet->token, "raikiri-1");
    ASSERT_TRUE(endpoint.has_value());
    EXPECT_EQ(endpoint->host, "ws-api-futures.kucoin.com");
    EXPECT_EQ(endpoint->port, "443");
    EXPECT_TRUE(endpoint->target.starts_with("/?token="));
    EXPECT_TRUE(endpoint->target.ends_with("&connectId=raikiri-1"));

    EXPECT_TRUE(make_endpoint("wss://host:9443/path", "t", "c").has_value());
    EXPECT_EQ(make_endpoint("wss://host:9443/path", "t", "c")->port, "9443");
    // Anything that could end the target or start another parameter would
    // connect as someone else's session.
    EXPECT_FALSE(make_endpoint("https://host/", "t", "c").has_value());
    EXPECT_FALSE(make_endpoint("wss://host/", "tok&x=1", "c").has_value());
    EXPECT_FALSE(make_endpoint("wss://host/", "", "c").has_value());
}

// ----------------------------------------------------------- errors ----

TEST(KucoinErrors, ClassificationPutsCodesInTheBucketTheGatewayActsOn) {
    EXPECT_EQ(classify(k_signature_error), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify(k_ip_not_whitelisted), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify(k_too_many_requests), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(k_internal_server_error), exec::ErrorClass::Retryable);
    // Clock skew against a 5 s window, not bad credentials: halting the venue
    // would be wrong when the keys are fine.
    EXPECT_EQ(classify(k_timestamp_invalid), exec::ErrorClass::Retryable);
    // An expired bullet token is issued by an UNAUTHENTICATED call, so it
    // needs a new token rather than a halted venue.
    EXPECT_EQ(classify(k_ws_token_invalid), exec::ErrorClass::Retryable);
    EXPECT_TRUE(is_expired_token(k_ws_token_invalid));
    EXPECT_FALSE(is_expired_token(k_ws_topic_not_found));
    // A refused topic cannot be retried into existence.
    EXPECT_EQ(classify(k_ws_topic_not_found), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_url_not_found), exec::ErrorClass::Terminal);
    // Unknown codes stay Terminal: an unknown refusal repeated at full speed
    // is how an account earns an IP ban.
    EXPECT_EQ(classify(999999), exec::ErrorClass::Terminal);
}

} // namespace
} // namespace raikiri::venues::kucoin
