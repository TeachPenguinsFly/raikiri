#include <raikiri/domain/asset_class.hpp>
#include <raikiri/venues/kucoin/account.hpp>
#include <raikiri/venues/kucoin/book.hpp>
#include <raikiri/venues/kucoin/catalogue.hpp>
#include <raikiri/venues/kucoin/envelope.hpp>
#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/venues/kucoin/kucoin_venue.hpp>
#include <raikiri/venues/kucoin/map.hpp>
#include <raikiri/venues/kucoin/market_data.hpp>
#include <raikiri/venues/kucoin/orders.hpp>
#include <raikiri/venues/kucoin/positions.hpp>
#include <raikiri/venues/kucoin/rest.hpp>
#include <raikiri/venues/kucoin/ws.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace raikiri::venues::kucoin {
namespace {

// Every parser here is zero-copy: the DTO it fills holds views INTO this
// string, so the returned body has to be bound to a named local that outlives
// them. Parsing a temporary leaves a wire struct pointing at freed memory.
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

// The spot table lives on the other host and comes from the other catalogue,
// so nothing about it is shared with the futures one but the loading path.
exec::InstrumentTable loaded_spot_table() {
    const auto body = fixture("rest_spot_symbols.json");
    static SpotSymbolBuffer storage;
    const auto wire = parse_spot_symbols(body, storage);
    EXPECT_TRUE(wire.has_value());
    exec::InstrumentTable table;
    for (const auto& entry : *wire) {
        if (auto instrument = map_spot_symbol(entry); instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value());
        }
    }
    return table;
}

// A signer loaded with values that are not a real key. Every signature pinned
// below was computed independently against these three strings.
constexpr std::string_view k_test_key = "kucoin-test-key";
constexpr std::string_view k_test_secret = "kucoin-test-secret";
constexpr std::string_view k_test_passphrase = "kucoin-test-passphrase";

void load_test_credentials(Credentials& credentials) {
    credentials.signer().set_api_key(k_test_key);
    ASSERT_TRUE(credentials.signer().secret().assign(k_test_secret).has_value());
    ASSERT_TRUE(credentials.seal_passphrase(k_test_passphrase));
}

std::string_view header_of(const net::RestRequest& request, std::string_view name) {
    for (const auto& header : request.headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return {};
}

OrderCmd limit_order(const exec::InstrumentTable& table,
                     std::string_view symbol,
                     std::string_view price,
                     std::string_view qty) {
    const auto* instrument = table.find(symbol);
    EXPECT_NE(instrument, nullptr) << symbol;
    if (instrument == nullptr) {
        return {}; // a caller naming an unloaded symbol fails here, not in a signal handler
    }
    const auto& base = base_of(*instrument);
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.side = Side::Buy;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.instrument = base.id;
    cmd.client_order_id = ClientOrderId::make(1, 7);
    cmd.exchange_client_order_id =
        ExchangeClientOrderId::from("01J8ZQK9V7M3XPB2R6TDNAWC5E").value_or(ExchangeClientOrderId{});
    cmd.price = PriceTicks::from_raw(parse_number_e9(price)->raw / base.tick_size.raw);
    cmd.qty = QtyLots::from_raw(parse_number_e9(qty)->raw / base.lot_size.raw);
    return cmd;
}

// Every signature in this file was taken at this instant; KuCoin signs
// milliseconds, so the nanoseconds below render as 1730211033335.
constexpr UnixNanos k_signed_at{1'730'211'033'335'000'000ULL};

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
        exec::make_venue({.venue = VenueId::Kucoin, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Kucoin);
    EXPECT_NE(adapter->data, nullptr);
    EXPECT_NE(adapter->execution, nullptr);
    EXPECT_NE(adapter->instruments, nullptr);
}

TEST(KucoinVenue, AnOrderThatCannotBeSignedIsRefusedRatherThanLeftOpen) {
    // No credentials are configured here, so nothing can be signed. Silence
    // would leave the order open in the store forever, waiting for a verdict
    // that will never be produced.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter =
        exec::make_venue({.venue = VenueId::Kucoin, .market = MarketType::Perp}, sink, clock);
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

    cmd.kind = CmdKind::Amend;
    adapter->execution->amend(cmd);
    ASSERT_EQ(sink.events.size(), 3U);
    EXPECT_EQ(sink.events[2].type, OrderEventType::ModifyRejected);
}

TEST(KucoinVenue, StartingWithoutTheFullCredentialSetIsRefused) {
    // KuCoin needs three secrets, not two. Starting on a partial set would let
    // every order fail one at a time, each looking like a venue problem.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter =
        exec::make_venue({.venue = VenueId::Kucoin, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    const auto started = adapter->execution->start();
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().cls, exec::ErrorClass::AuthFailure);
}

TEST(KucoinVenue, MarketsTheAdapterDoesNotServeAreNamedRefusals) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter =
        exec::make_venue({.venue = VenueId::Kucoin, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    // No network is touched: each market is refused before a request is built.
    // An empty list would read as "flat" or "nothing open", which are answers,
    // not refusals.
    EXPECT_FALSE(adapter->instruments->fetch_all(MarketType::Option).has_value());
    EXPECT_FALSE(adapter->instruments->fetch_all(MarketType::Future).has_value());
    EXPECT_FALSE(adapter->execution->open_order_reports(MarketType::Option).has_value());
    EXPECT_FALSE(adapter->execution->fill_reports(MarketType::Option, UnixNanos{0}).has_value());
    EXPECT_FALSE(adapter->execution->position_reports(MarketType::Option).has_value());
    EXPECT_FALSE(adapter->execution->balance_reports(MarketType::Option).has_value());
    // Spot was not enabled on this gateway, and spot has no positions at all.
    EXPECT_FALSE(adapter->execution->open_order_reports(MarketType::Spot).has_value());
    EXPECT_FALSE(adapter->execution->position_reports(MarketType::Spot).has_value());
    // Spot HAS a wallet, on the other host and in another shape. Refusing it
    // beats answering with the futures one, which is a different account.
    EXPECT_FALSE(adapter->execution->balance_reports(MarketType::Spot).has_value());
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
    const auto wire = parse_bbo(frame, MarketType::Perp);
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
    const auto wire = parse_bbo(frame, MarketType::Perp);
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
    const auto wire = parse_trade(frame, MarketType::Perp);
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
                           R"("subject":"tickerV2","data":{}})",
                           MarketType::Perp)
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
    // which is why the mapper reads it with the tolerant reader instead of the
    // exact one every order price goes through.
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
    const auto ok_body = fixture("rest_timestamp.json");
    const auto ok = parse_rest_envelope(ok_body);
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->code, k_ok);

    const auto missing_body = fixture("rest_error_unknown_symbol.json");
    const auto missing = parse_rest_envelope(missing_body);
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(missing->code, k_url_not_found);
    EXPECT_FALSE(missing->msg.empty());

    const auto unsupported_body = fixture("rest_error_unsupported.json");
    const auto unsupported = parse_rest_envelope(unsupported_body);
    ASSERT_TRUE(unsupported.has_value());
    EXPECT_EQ(unsupported->code, k_unsupported);

    // A code that cannot be read must never decay into success.
    EXPECT_FALSE(parse_rest_envelope(R"({"code":"oops","data":1})").has_value());
    EXPECT_FALSE(parse_rest_envelope(R"({"data":1})").has_value());
}

TEST(KucoinRest, ServerTimeReadsABareNumberPayload) {
    const auto body = fixture("rest_timestamp.json");
    const auto ts = parse_server_time(body);
    ASSERT_TRUE(ts.has_value());
    EXPECT_EQ(*ts, 1786394604446ULL);
}

TEST(KucoinRest, TheCatalogueRowCarriesFundingForTheUnifiedFeed) {
    // ON UTA v2 THIS ROW IS THE ONLY FUNDING SOURCE. The unified feed carries
    // books and nothing else, and until 2026-09-24 a kucoin gateway on it
    // wrote 0 bytes of funding — the subscription returned before it reached
    // any fallback. The catalogue poll now publishes these fields instead.
    //
    // The dated future states no funding at all, and must still parse: a
    // contract without funding is a catalogue row, not a malformed answer.
    static ContractBuffer storage;
    const auto body = fixture("rest_contracts_active.json");
    const auto wire = parse_contracts(body, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->size(), 4U);

    EXPECT_EQ((*wire)[0].symbol, "XBTUSDTM");
    // Java's formatting, kept as sent — the fixed-point reader parses the
    // exponent form, and rewriting it here would hide what the venue said.
    EXPECT_EQ((*wire)[0].funding_fee_rate, "1.0E-4");
    EXPECT_EQ((*wire)[0].next_funding_ms, 1786406400000ULL);
    EXPECT_EQ((*wire)[0].funding_granularity_ms, 28800000ULL);

    EXPECT_EQ((*wire)[2].symbol, "XBTMU26");
    EXPECT_TRUE((*wire)[2].funding_fee_rate.empty());
    EXPECT_EQ((*wire)[2].next_funding_ms, 0ULL);
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

TEST(KucoinMap, InversePerpetualIsRefusedByItsFlagNotItsMultiplier) {
    static ContractBuffer storage;
    const auto body = fixture("rest_contracts_inverse_perp.json");
    const auto wire = parse_contracts(body, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->size(), 1U);
    EXPECT_EQ((*wire)[0].symbol, "XBTUSDM");
    EXPECT_EQ((*wire)[0].type, "FFWCSX");
    EXPECT_TRUE((*wire)[0].is_inverse);
    EXPECT_FALSE(map_instrument((*wire)[0], MarketType::Perp).has_value());

    auto positive = (*wire)[0];
    positive.multiplier = "1";
    EXPECT_FALSE(map_instrument(positive, MarketType::Perp).has_value());
    positive.is_inverse = false;
    EXPECT_TRUE(map_instrument(positive, MarketType::Perp).has_value());
}

TEST(KucoinMap, XbtusdtmIsTheSameMarketAsEveryOtherVenuesBtcUsdt) {
    // The row the XBT alias exists for: KuCoin spells bitcoin the ISO way in
    // both the symbol and the field, and an XBT/USDT here would group with
    // nothing on the four venues that call it BTC.
    static ContractBuffer storage;
    const auto body = fixture("rest_contracts_active.json");
    const auto wire = parse_contracts(body, storage);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ((*wire)[0].base_currency, "XBT");

    const auto xbt = map_instrument((*wire)[0], MarketType::Perp);
    ASSERT_TRUE(xbt.has_value());
    EXPECT_EQ(base_of(*xbt).exchange_symbol.view(), "XBTUSDTM");
    EXPECT_EQ(base_of(*xbt).local_symbol.view(), "BTC/USDT");

    // A row naming no pair is refused rather than carried under XBTUSDTM: an
    // instrument nothing can group shows as a market of its own.
    auto unnamed = (*wire)[0];
    unnamed.base_currency = {};
    EXPECT_FALSE(map_instrument(unnamed, MarketType::Perp).has_value());

    // The spot symbol carries the pair itself, where XBTUSDTM carries none.
    static SpotSymbolBuffer spot_storage;
    const auto spot_body = fixture("rest_spot_symbols.json");
    const auto spot = parse_spot_symbols(spot_body, spot_storage);
    ASSERT_TRUE(spot.has_value());
    const auto btc = map_spot_symbol((*spot)[0]);
    ASSERT_TRUE(btc.has_value());
    EXPECT_EQ(base_of(*btc).exchange_symbol.view(), "BTC-USDT");
    EXPECT_EQ(base_of(*btc).local_symbol.view(), "BTC/USDT");
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
    const auto wire = parse_bbo(frame, MarketType::Perp);
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
    auto wire = parse_trade(frame, MarketType::Perp);
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
    FundingMemo memo{};
    const auto body = fixture("rest_funding_rate_current.json");
    const auto wire = parse_funding_current(body);
    ASSERT_TRUE(wire.has_value());
    const auto funding = map_funding(*wire, "XBTUSDTM", table, memo, UnixNanos{11});
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 100'000); // 1.0E-4
    EXPECT_EQ(funding->interval_s, 28800U);
    EXPECT_EQ(funding->next_funding_time.ns, 1786406400000ULL * 1'000'000ULL);
    EXPECT_EQ(funding->ts_event.ns, 1786377600000ULL * 1'000'000ULL);
    // Undefined until the mark subject says otherwise: a zero would read as a
    // real price of zero.
    EXPECT_TRUE(funding->mark_price.is_undef());
    // The symbol argument wins, because the body names the funding index.
    EXPECT_FALSE(map_funding(*wire, "NOSUCHSYM", table, memo, UnixNanos{11}).has_value());
}

TEST(KucoinMap, FundingFromTheStreamIgnoresItsOwnGranularity) {
    const auto table = loaded_table();
    FundingMemo memo{};
    const auto frame = fixture("ws_funding_rate.json");
    auto wire = parse_funding_rate(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->rate, "0.000100");
    // The stream republishes once a minute and reports that cadence here,
    // where the REST body reports the 28800000 settlement period instead.
    EXPECT_EQ(wire->granularity_ms, 60'000ULL);
    EXPECT_EQ(wire->next_funding_ms, 0ULL);

    const auto funding = map_funding(*wire, wire->symbol, table, memo, UnixNanos{5});
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
    EXPECT_FALSE(map_funding(*wire, wire->symbol, table, memo, UnixNanos{5}).has_value());
}

TEST(KucoinMapFunding, TheMarkArrivesUnderItsOwnSubjectAndJoinsTheSameRecord) {
    // KuCoin splits what FundingUpdate fuses: funding.rate and
    // mark.index.price share one topic and arrive as separate frames. The shm
    // cell is written whole, so publishing straight from either frame writes
    // UNDEF over what the other one said.
    const auto table = loaded_table();
    FundingMemo memo{};

    const auto rate_frame = fixture("ws_funding_rate.json");
    const auto rate = parse_funding_rate(rate_frame);
    ASSERT_TRUE(rate.has_value());
    const auto first = map_funding(*rate, rate->symbol, table, memo, UnixNanos{5});
    ASSERT_TRUE(first.has_value());
    EXPECT_TRUE(first->mark_price.is_undef()) << "no mark has been stated yet";

    const auto mark_frame = fixture("ws_mark_index_price.json");
    const auto mark = parse_mark_price(mark_frame);
    ASSERT_TRUE(mark.has_value());
    const auto both = map_mark_price(*mark, table, memo, UnixNanos{6});
    ASSERT_TRUE(both.has_value());
    // 64175.21 dropped to the contract's 0.1 tick: the mark is quoted on a
    // finer grid than the contract trades on.
    EXPECT_EQ(both->mark_price.raw, 641'752);
    EXPECT_EQ(both->rate.raw, 100'000) << "the rate the other subject stated is carried forward";
    EXPECT_EQ(both->interval_s, 28800U);
}

TEST(KucoinMapFunding, ARateFrameDoesNotEraseAMarkAlreadyStated) {
    const auto table = loaded_table();
    FundingMemo memo{};

    const auto mark_frame = fixture("ws_mark_index_price.json");
    const auto mark = parse_mark_price(mark_frame);
    ASSERT_TRUE(mark.has_value());
    ASSERT_TRUE(map_mark_price(*mark, table, memo, UnixNanos{5}).has_value());
    const auto held = memo.mark;

    const auto rate_frame = fixture("ws_funding_rate.json");
    const auto rate = parse_funding_rate(rate_frame);
    ASSERT_TRUE(rate.has_value());
    const auto after = map_funding(*rate, rate->symbol, table, memo, UnixNanos{6});
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->mark_price.raw, held.raw) << "the mark survives a frame that omitted it";
}

// --------------------------------------------------------------- ws ----

// --------------------------------------------------------------- book ----

// The parsed views point into the body text and into the level buffer, so both
// have to outlive the wire struct that came out of them.
struct SeedFrame {
    std::string text;
    std::unique_ptr<LevelBuffer> storage{std::make_unique<LevelBuffer>()};
};

WireOrderBook seed_of(SeedFrame& frame) {
    frame.text = fixture("rest_level2_snapshot.json");
    const auto wire = parse_level2_snapshot(frame.text, *frame.storage);
    EXPECT_TRUE(wire.has_value());
    return wire.value_or(WireOrderBook{});
}

// The change parser hands back views into the frame text alone, so the caller
// only has to keep that alive.
WireBookChange change_of(std::string& text, std::string_view name) {
    text = fixture(name);
    const auto wire = parse_book_change(text);
    EXPECT_TRUE(wire.has_value());
    return wire.value_or(WireBookChange{});
}

DepthBook book_for(const exec::InstrumentTable& table) {
    const auto* instrument = table.find("XBTUSDTM");
    EXPECT_NE(instrument, nullptr);
    const auto& base = base_of(*instrument);
    return DepthBook{base.id, base.tick_size, base.lot_size};
}

// Bids descend, asks ascend, the two never cross and every quantity is real.
// A book that fails this is not a book, and publishing it would price every
// strategy reading shared memory.
void expect_coherent(const BookSnapshot& out) {
    ASSERT_GT(out.bid_count, 0U);
    ASSERT_GT(out.ask_count, 0U);
    EXPECT_LE(out.bid_count, k_book_levels);
    EXPECT_LE(out.ask_count, k_book_levels);
    EXPECT_LT(out.bids[0].price.raw, out.asks[0].price.raw);
    for (std::uint32_t i = 1; i < out.bid_count; ++i) {
        EXPECT_LT(out.bids[i].price.raw, out.bids[i - 1].price.raw) << i;
    }
    for (std::uint32_t i = 1; i < out.ask_count; ++i) {
        EXPECT_GT(out.asks[i].price.raw, out.asks[i - 1].price.raw) << i;
    }
    for (std::uint32_t i = 0; i < out.bid_count; ++i) {
        EXPECT_GT(out.bids[i].qty.raw, 0) << i;
    }
    for (std::uint32_t i = 0; i < out.ask_count; ++i) {
        EXPECT_GT(out.asks[i].qty.raw, 0) << i;
    }
}

TEST(KucoinBook, AnUnseededBookPublishesNothing) {
    // There is no snapshot on this topic at all, and one frame is one level, so
    // an adapter that trusted the stream alone would quote a two-level market.
    const auto table = loaded_table();
    auto book = book_for(table);
    std::string text;
    EXPECT_TRUE(book.wants_seed(0));
    EXPECT_EQ(book.apply(change_of(text, "ws_level2_change.json")), BookResult::Buffered);
    EXPECT_FALSE(book.live());
    EXPECT_FALSE(book.snapshot(UnixNanos{1}, UnixNanos{1}).has_value());
}

TEST(KucoinBook, SeedAndStreamMeetInABookSharedMemoryCanTake) {
    const auto table = loaded_table();
    auto book = book_for(table);
    SeedFrame seed_frame;
    std::string first;

    const auto seed = seed_of(seed_frame);
    EXPECT_EQ(seed.bids.size(), 1000U);
    EXPECT_EQ(seed.asks.size(), 1000U);
    EXPECT_EQ(book.seed(seed), BookResult::Waiting);
    EXPECT_FALSE(book.snapshot(UnixNanos{1}, UnixNanos{1}).has_value());

    const auto d1 = change_of(first, "ws_level2_change.json");
    // The chain is contiguous, so the frame that continues the seed is the one
    // holding sequence + 1 — not the one holding sequence, already in the levels.
    EXPECT_EQ(d1.sequence, seed.sequence + 1);
    EXPECT_EQ(book.apply(d1), BookResult::Applied);

    const auto out = book.snapshot(UnixNanos{7}, UnixNanos{9});
    ASSERT_TRUE(out.has_value());
    expect_coherent(*out);
    EXPECT_EQ(out->bid_count, k_book_levels);
    EXPECT_EQ(out->ask_count, k_book_levels);
    EXPECT_EQ(out->bids[0].price.raw, 640'903); // 64090.3 over the 0.1 tick
    EXPECT_EQ(out->asks[0].price.raw, 640'904);
    EXPECT_EQ(out->bids[0].qty.raw, 2); // contracts, over a lot step of 1
    EXPECT_EQ(out->bids[99].price.raw, 640'504);
    EXPECT_EQ(out->asks[99].price.raw, 641'244);
    EXPECT_EQ(out->venue_seq, static_cast<std::uint64_t>(d1.sequence));
    EXPECT_EQ(out->ts_event.ns, 7ULL);
    EXPECT_EQ(out->ts_init.ns, 9ULL);
    // The change was a new bid 871 levels down, far below what shm carries but
    // still inside the book the seed vouched for.
    EXPECT_EQ(book.bids().size(), 1001U);
}

TEST(KucoinBook, ChangesBufferedBehindTheSeedReplayIntoTheSameBook) {
    // Which of the REST answer and the websocket frame lands first is a race,
    // and the book it produces must not depend on who won.
    const auto table = loaded_table();
    SeedFrame seed_frame;
    std::string first;
    std::string second;
    const auto seed = seed_of(seed_frame);
    const auto d1 = change_of(first, "ws_level2_change.json");
    const auto d2 = change_of(second, "ws_level2_change_next.json");

    auto streamed = book_for(table);
    EXPECT_EQ(streamed.seed(seed), BookResult::Waiting);
    EXPECT_EQ(streamed.apply(d1), BookResult::Applied);
    EXPECT_EQ(streamed.apply(d2), BookResult::Applied);

    auto buffered = book_for(table);
    EXPECT_EQ(buffered.apply(d1), BookResult::Buffered);
    EXPECT_EQ(buffered.apply(d2), BookResult::Buffered);
    EXPECT_EQ(buffered.seed(seed), BookResult::Applied);

    const auto from_stream = streamed.snapshot(UnixNanos{1}, UnixNanos{1});
    const auto from_buffer = buffered.snapshot(UnixNanos{1}, UnixNanos{1});
    ASSERT_TRUE(from_stream.has_value());
    ASSERT_TRUE(from_buffer.has_value());
    expect_coherent(*from_buffer);
    EXPECT_EQ(from_buffer->venue_seq, from_stream->venue_seq);
    for (std::uint32_t i = 0; i < k_book_levels; ++i) {
        EXPECT_EQ(from_buffer->bids[i].price.raw, from_stream->bids[i].price.raw) << i;
        EXPECT_EQ(from_buffer->bids[i].qty.raw, from_stream->bids[i].qty.raw) << i;
        EXPECT_EQ(from_buffer->asks[i].price.raw, from_stream->asks[i].price.raw) << i;
        EXPECT_EQ(from_buffer->asks[i].qty.raw, from_stream->asks[i].qty.raw) << i;
    }
}

TEST(KucoinBook, ALevelOutsideTheSeedsBandIsNotCarried) {
    // A real frame off the live socket: a bid at 0.1, six thousand levels below
    // the deepest price the seed vouched for. Nothing confirms it is still
    // there, so carrying it would make the book look deeper than it is known
    // to be — and would put a bid of 0.1 in shm.
    const auto table = loaded_table();
    auto book = book_for(table);
    SeedFrame seed_frame;
    std::string first;
    std::string second;
    EXPECT_EQ(book.seed(seed_of(seed_frame)), BookResult::Waiting);
    EXPECT_EQ(book.apply(change_of(first, "ws_level2_change.json")), BookResult::Applied);
    const auto held = book.bids().size();

    const auto far = change_of(second, "ws_level2_change_next.json");
    EXPECT_EQ(far.price, "0.1");
    EXPECT_EQ(book.apply(far), BookResult::Applied);
    EXPECT_EQ(book.bids().size(), held);
    EXPECT_EQ(book.bids().back().price.raw, 617'388) << "61738.8, the seed's own floor";
}

TEST(KucoinBook, ASizeOfZeroDeletesTheLevel) {
    const auto table = loaded_table();
    auto book = book_for(table);
    SeedFrame seed_frame;
    std::string text;
    const auto seed = seed_of(seed_frame);
    EXPECT_EQ(book.seed(seed), BookResult::Waiting);

    // A real delete off the same capture, moved to the seam so it is the frame
    // that continues the seed; seven frames at prices outside the band sat
    // between them live.
    auto gone = change_of(text, "ws_level2_delete.json");
    EXPECT_EQ(gone.qty, "0");
    gone.sequence = seed.sequence + 1;

    EXPECT_EQ(book.apply(gone), BookResult::Applied);
    const auto out = book.snapshot(UnixNanos{1}, UnixNanos{1});
    ASSERT_TRUE(out.has_value());
    expect_coherent(*out);
    EXPECT_EQ(out->asks[5].price.raw, 640'933) << "64093.3 stepped up into the slot";
    EXPECT_EQ(book.asks().size(), 999U) << "a zero deletes, it does not rest at zero";
}

TEST(KucoinBook, AGapInTheChainPublishesNothing) {
    const auto table = loaded_table();
    auto book = book_for(table);
    SeedFrame seed_frame;
    std::string first;
    std::string second;
    EXPECT_EQ(book.seed(seed_of(seed_frame)), BookResult::Waiting);
    EXPECT_EQ(book.apply(change_of(first, "ws_level2_change.json")), BookResult::Applied);

    auto skipped = change_of(second, "ws_level2_change_next.json");
    skipped.sequence += 1; // one frame of the chain went missing

    EXPECT_EQ(book.apply(skipped), BookResult::Gap);
    EXPECT_EQ(book.gaps(), 1U);
    EXPECT_FALSE(book.live());
    EXPECT_FALSE(book.snapshot(UnixNanos{1}, UnixNanos{1}).has_value());
    EXPECT_TRUE(book.wants_seed(0)) << "a dropped book asks for a new seed";
}

// THE RETRY FLOOR, which is what stops a refused seed being asked for again on
// the very next frame. Without it 677 books asked 27391 times in seven minutes
// and were refused 22861 of them with 429.
TEST(KucoinBook, ARefusedSeedWaitsBeforeAskingAgain) {
    DepthBook book{InstrumentId{1}, FixedE9{1}, FixedE9{1}};
    constexpr std::uint64_t now = 1'000'000'000'000ULL;
    EXPECT_TRUE(book.wants_seed(now)) << "a cold book asks straight away";
    book.seed_requested();
    EXPECT_FALSE(book.wants_seed(now)) << "one in flight is not two";
    book.seed_failed(now);
    EXPECT_FALSE(book.wants_seed(now)) << "refused, and the next frame must not re-ask";
    EXPECT_FALSE(book.wants_seed(now + DepthBook::k_seed_retry_ns - 1));
    EXPECT_TRUE(book.wants_seed(now + DepthBook::k_seed_retry_ns));
}

TEST(KucoinBook, ASeedTheStreamHasRunPastIsDiscarded) {
    // Measured live: a snapshot fetched alongside a fresh subscription came
    // back 1555 sequences behind the first streamed frame. The chain is
    // contiguous, so those changes are gone — splicing the seed in anyway
    // would produce a book missing every one of them.
    const auto table = loaded_table();
    auto book = book_for(table);
    SeedFrame seed_frame;
    std::string first;
    auto seed = seed_of(seed_frame);
    const auto d1 = change_of(first, "ws_level2_change.json");
    seed.sequence = d1.sequence - 2;

    EXPECT_EQ(book.seed(seed), BookResult::Waiting);
    EXPECT_EQ(book.apply(d1), BookResult::Reseed);
    EXPECT_FALSE(book.live());
    EXPECT_FALSE(book.snapshot(UnixNanos{1}, UnixNanos{1}).has_value());
    EXPECT_TRUE(book.wants_seed(0));
}

TEST(KucoinBook, ALevelOffTheStepInvalidatesTheWholeBook) {
    // The rule that separates a delta venue from a snapshot venue. Dropping one
    // level here would leave a book that disagrees with KuCoin's forever, with
    // nothing downstream able to see the difference.
    const auto table = loaded_table();
    auto book = book_for(table);
    SeedFrame seed_frame;
    const auto seed = seed_of(seed_frame);
    EXPECT_EQ(book.seed(seed), BookResult::Waiting);

    WireBookChange off_tick{};
    off_tick.symbol = "XBTUSDTM";
    off_tick.price = "64090.35"; // finer than the 0.1 tick KuCoin states
    off_tick.qty = "1";
    off_tick.bid = true;
    off_tick.sequence = seed.sequence + 1;
    EXPECT_EQ(book.apply(off_tick), BookResult::BadLevel);
    EXPECT_FALSE(book.live());
    EXPECT_FALSE(book.snapshot(UnixNanos{1}, UnixNanos{1}).has_value());

    // And a fraction of a contract is refused the same way.
    auto again = book_for(table);
    EXPECT_EQ(again.seed(seed), BookResult::Waiting);
    WireBookChange sub_lot{};
    sub_lot.symbol = "XBTUSDTM";
    sub_lot.price = "64090.3";
    sub_lot.qty = "0.5";
    sub_lot.bid = true;
    sub_lot.sequence = seed.sequence + 1;
    EXPECT_EQ(again.apply(sub_lot), BookResult::BadLevel);
    EXPECT_FALSE(again.snapshot(UnixNanos{1}, UnixNanos{1}).has_value());
}

TEST(KucoinBook, SnapshotWithoutASequenceIsNotASeed) {
    // Without it the levels cannot be placed in the change chain, and a book
    // that cannot be continued is a photograph that will never move again.
    auto storage = std::make_unique<LevelBuffer>();
    const std::string body =
        R"({"code":"200000","data":{"symbol":"XBTUSDTM","bids":[[1.0,1]],"asks":[[2.0,1]],"ts":1}})";
    EXPECT_FALSE(parse_level2_snapshot(body, *storage).has_value());
}

TEST(KucoinWs, TopicsCoverTheKindsKucoinStreams) {
    // tickerV2 AND NOT THE BOOK'S TOUCH, which was tried and reverted: the
    // touch exists only once a book is Live, and a contract whose book never
    // moves never seeds. Coverage fell 97% to 78%. See topic_for().
    EXPECT_EQ(topic_for(exec::SubKind::Bbo, MarketType::Perp), "/contractMarket/tickerV2");
    EXPECT_EQ(topic_for(exec::SubKind::Depth, MarketType::Perp), "/contractMarket/level2");
    EXPECT_EQ(topic_for(exec::SubKind::Trades, MarketType::Perp), "/contractMarket/execution");
    EXPECT_EQ(topic_for(exec::SubKind::Depth, MarketType::Perp), "/contractMarket/level2");
    // Mark and funding share one topic; `subject` separates them.
    EXPECT_EQ(topic_for(exec::SubKind::Funding, MarketType::Perp), "/contract/instrument");
    EXPECT_EQ(topic_for(exec::SubKind::MarkPrice, MarketType::Perp), "/contract/instrument");
    EXPECT_TRUE(topic_for(exec::SubKind::Kline, MarketType::Perp).empty());
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

// ------------------------------------------------------- order entry ----

TEST(KucoinSign, TheSignedStringIsTheRequestLineThatGoesOut) {
    Credentials credentials;
    load_test_credentials(credentials);
    OrderRequestBuilder builder{credentials};
    const auto table = loaded_spot_table();

    const auto request = builder.create_order(
        limit_order(table, "BTC-USDT", "50000", "0.00001"), table, k_signed_at);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());
    EXPECT_EQ(request->target, "/api/v1/hf/orders");
    EXPECT_EQ(request->body,
              R"({"clientOid":"01J8ZQK9V7M3XPB2R6TDNAWC5E","symbol":"BTC-USDT","side":"buy",)"
              R"("type":"limit","size":"0.00001","price":"50000","timeInForce":"GTC"})");
    // Milliseconds, method upper case, path then body, nothing between them.
    EXPECT_EQ(builder.last_sign_message(),
              std::string{"1730211033335"} + "POST" + "/api/v1/hf/orders" +
                  std::string{request->body});
    EXPECT_EQ(header_of(*request, "KC-API-TIMESTAMP"), "1730211033335");
    EXPECT_EQ(header_of(*request, "KC-API-KEY"), k_test_key);
    EXPECT_EQ(header_of(*request, "KC-API-SIGN"), "+AWhvqpmFDu/7vY0xCCjfBIh4UC+cORP7LBx6rOlQTU=");
}

TEST(KucoinSign, KeyVersionTwoSendsTheHashedPassphraseNeverTheSecretItself) {
    // A passphrase sent in clear answers 400004, which reads like the wrong
    // passphrase rather than the wrong encoding, and costs an afternoon.
    Credentials credentials;
    load_test_credentials(credentials);
    OrderRequestBuilder builder{credentials};

    const auto request = builder.positions(MarketType::Perp, k_signed_at);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(header_of(*request, "KC-API-KEY-VERSION"), "2");
    EXPECT_EQ(header_of(*request, "KC-API-PASSPHRASE"),
              "WX/MCPps2NpCAWajYjOod/NfgMDWvFoHPQ15hoWy+fE=");
    EXPECT_NE(header_of(*request, "KC-API-PASSPHRASE"), k_test_passphrase);
}

TEST(KucoinSign, AGetSignsItsQueryStringAndSendsNoBody) {
    Credentials credentials;
    load_test_credentials(credentials);
    OrderRequestBuilder builder{credentials};

    const auto request = builder.open_orders(MarketType::Perp, {}, k_signed_at);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->method, net::HttpMethod::Get);
    // Without status=active KuCoin answers with the DONE orders, which a
    // reconciler reads as a book full of orders that are already gone.
    EXPECT_EQ(request->target, "/api/v1/orders?status=active");
    EXPECT_TRUE(request->body.empty());
    EXPECT_EQ(builder.last_sign_message(), "1730211033335GET/api/v1/orders?status=active");
    EXPECT_EQ(header_of(*request, "KC-API-SIGN"), "rsS89SYWmBw2x3pvf1TSzXJzmxf+PjBwPgX6vM8khlY=");
}

TEST(KucoinSign, WithoutCredentialsNothingIsBuiltAtAll) {
    // An unsigned order is not a degraded order; it is a request the venue
    // answers with an auth error that looks like a bad key.
    Credentials credentials;
    OrderRequestBuilder builder{credentials};
    const auto table = loaded_spot_table();
    EXPECT_EQ(
        builder.create_order(limit_order(table, "BTC-USDT", "50000", "0.00001"), table, k_signed_at)
            .error(),
        BuildError::NotSigned);
    EXPECT_EQ(builder.positions(MarketType::Perp, k_signed_at).error(), BuildError::NotSigned);

    // A secret alone is not enough: the passphrase is a third credential.
    credentials.signer().set_api_key(k_test_key);
    ASSERT_TRUE(credentials.signer().secret().assign(k_test_secret).has_value());
    EXPECT_FALSE(credentials.complete());
    EXPECT_EQ(builder.positions(MarketType::Perp, k_signed_at).error(), BuildError::NotSigned);
}

TEST(KucoinRest, SpotAndFuturesAreDifferentHostsAndDifferentPayloads) {
    EXPECT_EQ(rest_host(MarketType::Spot), "api.kucoin.com");
    EXPECT_EQ(rest_host(MarketType::Perp), "api-futures.kucoin.com");
    EXPECT_EQ(rest_host(MarketType::Future), "api-futures.kucoin.com");
    EXPECT_TRUE(rest_host(MarketType::Option).empty());
    EXPECT_EQ(rest_host(), rest_host(MarketType::Perp));

    Credentials credentials;
    load_test_credentials(credentials);
    OrderRequestBuilder builder{credentials};
    const auto futures = loaded_table();

    const auto request = builder.create_order(
        limit_order(futures, "XBTUSDTM", "64125.6", "3"), futures, k_signed_at);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());
    EXPECT_EQ(request->target, "/api/v1/orders");
    // `size` is a JSON NUMBER of contracts here and a quoted decimal of base
    // units on spot. There is no `leverage`: it is optional, and it SETS the
    // position's margin, so inventing one chooses risk nobody asked for.
    EXPECT_EQ(request->body,
              R"({"clientOid":"01J8ZQK9V7M3XPB2R6TDNAWC5E","symbol":"XBTUSDTM","side":"buy",)"
              R"("type":"limit","size":3,"price":"64125.6","timeInForce":"GTC"})");
    EXPECT_EQ(header_of(*request, "KC-API-SIGN"), "3ADpi9CieKkK4oCK+4ulPbtG5GP7eke2REbxhb52wX4=");
}

TEST(KucoinRest, FuturesRefusesAQuantityThatIsNotWholeContracts) {
    // KuCoin trades lots. Rounding a fraction of one is how an order reaches
    // the venue at a size nobody chose.
    Credentials credentials;
    load_test_credentials(credentials);
    OrderRequestBuilder builder{credentials};
    const auto table = loaded_table();

    // PEPEUSDTM is in the catalogue and not in the table: its tick is a decade
    // below FixedE9, so the mapper refused the row. An order naming it can only
    // carry an id that resolves to nothing.
    ASSERT_EQ(table.find("PEPEUSDTM"), nullptr);
    auto cmd = limit_order(table, "XBTUSDTM", "64125.6", "3");
    cmd.instrument = InstrumentId::make(
        VenueId::Kucoin, MarketType::Perp, SymbolId{static_cast<std::uint32_t>(table.size())});
    EXPECT_EQ(builder.create_order(cmd, table, k_signed_at).error(), BuildError::UnknownInstrument);

    cmd = limit_order(table, "XBTUSDTM", "64125.6", "3");
    cmd.qty = QtyLots::from_raw(0);
    EXPECT_EQ(builder.create_order(cmd, table, k_signed_at).error(), BuildError::BadQuantity);
    cmd.qty = QtyLots::undef();
    EXPECT_EQ(builder.create_order(cmd, table, k_signed_at).error(), BuildError::BadQuantity);

    cmd = limit_order(table, "XBTUSDTM", "64125.6", "3");
    cmd.price = PriceTicks::undef();
    EXPECT_EQ(builder.create_order(cmd, table, k_signed_at).error(), BuildError::BadPrice);
}

TEST(KucoinRest, FlagsKucoinCannotExpressAreRefusedRatherThanDropped) {
    Credentials credentials;
    load_test_credentials(credentials);
    OrderRequestBuilder builder{credentials};
    const auto spot = loaded_spot_table();
    const auto futures = loaded_table();

    // Spot holds no position to reduce; dropping the flag would send an
    // opening order where a closing one was asked for.
    auto cmd = limit_order(spot, "BTC-USDT", "50000", "0.00001");
    cmd.flags = OrderFlags::ReduceOnly;
    EXPECT_EQ(builder.create_order(cmd, spot, k_signed_at).error(), BuildError::UnsupportedRequest);

    // Post-only is a boolean alongside a resting time in force; with IOC the
    // order is neither, so the pair is refused instead of quietly reshaped.
    cmd.flags = OrderFlags::PostOnly;
    cmd.tif = TimeInForce::Ioc;
    EXPECT_EQ(builder.create_order(cmd, spot, k_signed_at).error(), BuildError::UnsupportedRequest);

    auto perp = limit_order(futures, "XBTUSDTM", "64125.6", "3");
    perp.flags = OrderFlags::ReduceOnly | OrderFlags::PostOnly;
    const auto request = builder.create_order(perp, futures, k_signed_at);
    ASSERT_TRUE(request.has_value());
    EXPECT_NE(request->body.find(R"("postOnly":true)"), std::string_view::npos);
    EXPECT_NE(request->body.find(R"("reduceOnly":true)"), std::string_view::npos);
}

TEST(KucoinRest, CancelNamesTheSymbolOnEveryPathThatDemandsIt) {
    Credentials credentials;
    load_test_credentials(credentials);
    OrderRequestBuilder builder{credentials};
    const auto spot = loaded_spot_table();
    const auto futures = loaded_table();

    auto cmd = limit_order(spot, "BTC-USDT", "50000", "0.00001");
    cmd.kind = CmdKind::Cancel;
    auto request = builder.cancel_order(cmd, spot, k_signed_at);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->method, net::HttpMethod::Delete);
    EXPECT_EQ(request->target,
              "/api/v1/hf/orders/client-order/01J8ZQK9V7M3XPB2R6TDNAWC5E?symbol=BTC-USDT");

    // A venue order id, where we hold one, is the authoritative handle.
    cmd.target_exchange_order_id = ExchangeOrderId::from("67120bbef094e200070976f6").value();
    request = builder.cancel_order(cmd, spot, k_signed_at);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->target, "/api/v1/hf/orders/67120bbef094e200070976f6?symbol=BTC-USDT");

    auto perp = limit_order(futures, "XBTUSDTM", "64125.6", "3");
    perp.kind = CmdKind::Cancel;
    request = builder.cancel_order(perp, futures, k_signed_at);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->target,
              "/api/v1/orders/client-order/01J8ZQK9V7M3XPB2R6TDNAWC5E?symbol=XBTUSDTM");
    // Futures cancel-by-id needs no symbol, and adding one is a 400100.
    perp.target_exchange_order_id = ExchangeOrderId::from("230181737576050688").value();
    request = builder.cancel_order(perp, futures, k_signed_at);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->target, "/api/v1/orders/230181737576050688");
}

TEST(KucoinRest, AmendExistsOnSpotAndNowhereElse) {
    Credentials credentials;
    load_test_credentials(credentials);
    OrderRequestBuilder builder{credentials};
    const auto spot = loaded_spot_table();
    const auto futures = loaded_table();

    auto cmd = limit_order(spot, "BTC-USDT", "50000", "0.00001");
    cmd.kind = CmdKind::Amend;
    const auto request = builder.amend_order(cmd, spot, k_signed_at);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->target, "/api/v1/hf/orders/alter");
    EXPECT_EQ(request->body,
              R"({"symbol":"BTC-USDT","clientOid":"01J8ZQK9V7M3XPB2R6TDNAWC5E",)"
              R"("newPrice":"50000","newSize":"0.00001"})");

    // KuCoin's modify REPLACES both fields, so an amend naming only one cannot
    // be expressed; filling the other from memory resubmits a stale price.
    cmd.price = PriceTicks::undef();
    EXPECT_EQ(builder.amend_order(cmd, spot, k_signed_at).error(), BuildError::BadPrice);

    auto perp = limit_order(futures, "XBTUSDTM", "64125.6", "3");
    perp.kind = CmdKind::Amend;
    EXPECT_EQ(builder.amend_order(perp, futures, k_signed_at).error(),
              BuildError::UnsupportedRequest);
}

TEST(KucoinRest, TheSweepIsAccountWideOnFuturesAndPerSymbolOnSpot) {
    Credentials credentials;
    load_test_credentials(credentials);
    OrderRequestBuilder builder{credentials};

    EXPECT_EQ(builder.open_orders(MarketType::Spot, "BTC-USDT", k_signed_at)->target,
              "/api/v1/hf/orders/active?symbol=BTC-USDT");
    // An empty symbol would fetch nothing and read as "no open orders".
    EXPECT_EQ(builder.open_orders(MarketType::Spot, {}, k_signed_at).error(),
              BuildError::UnsupportedRequest);
    EXPECT_EQ(builder.active_symbols(MarketType::Spot, k_signed_at)->target,
              "/api/v1/hf/orders/active/symbols");
    EXPECT_EQ(builder.active_symbols(MarketType::Perp, k_signed_at).error(),
              BuildError::UnsupportedRequest);

    EXPECT_EQ(builder.fills(MarketType::Spot, UnixNanos{0}, k_signed_at)->target,
              "/api/v1/hf/fills");
    EXPECT_EQ(builder.fills(MarketType::Perp, UnixNanos{1'727'692'804'808'000'000ULL}, k_signed_at)
                  ->target,
              "/api/v1/fills?startAt=1727692804808");
    EXPECT_EQ(builder.positions(MarketType::Perp, k_signed_at)->target, "/api/v1/positions");
    // Spot has balances, not positions; an empty list would read as flat.
    EXPECT_EQ(builder.positions(MarketType::Spot, k_signed_at).error(),
              BuildError::UnsupportedRequest);
    // The private handshake is a signed POST, unlike the public one.
    const auto bullet = builder.bullet_private(MarketType::Spot, k_signed_at);
    ASSERT_TRUE(bullet.has_value());
    EXPECT_EQ(bullet->method, net::HttpMethod::Post);
    EXPECT_EQ(bullet->target, "/api/v1/bullet-private");
    // The spot catalogue is the one public read that is not on the futures host.
    RequestBuilder public_builder;
    EXPECT_EQ(public_builder.spot_symbols().target, "/api/v2/symbols");
}

TEST(KucoinRest, EveryBuilderRefusesOptionsByName) {
    // KuCoin lists no options on the APIs in scope. Each switch names the case
    // rather than defaulting, so listing them would break the build here first.
    Credentials credentials;
    load_test_credentials(credentials);
    OrderRequestBuilder builder{credentials};
    const auto table = loaded_table();

    auto cmd = limit_order(table, "XBTUSDTM", "64125.6", "3");
    cmd.instrument = InstrumentId::make(VenueId::Kucoin, MarketType::Option, SymbolId{0});
    // The market is named before the table is read. InstrumentId resolves by
    // symbol index alone, so index 0 finds the first PERPETUAL; a refusal that
    // came from the lookup would be reporting the wrong fact about this id.
    EXPECT_EQ(builder.create_order(cmd, table, k_signed_at).error(), BuildError::UnsupportedMarket);
    EXPECT_EQ(builder.cancel_order(cmd, table, k_signed_at).error(), BuildError::UnsupportedMarket);
    EXPECT_EQ(builder.amend_order(cmd, table, k_signed_at).error(), BuildError::UnsupportedMarket);
    EXPECT_EQ(builder.open_orders(MarketType::Option, "X", k_signed_at).error(),
              BuildError::UnsupportedMarket);
    EXPECT_EQ(builder.active_symbols(MarketType::Option, k_signed_at).error(),
              BuildError::UnsupportedMarket);
    EXPECT_EQ(builder.fills(MarketType::Option, UnixNanos{0}, k_signed_at).error(),
              BuildError::UnsupportedMarket);
    EXPECT_EQ(builder.positions(MarketType::Option, k_signed_at).error(),
              BuildError::UnsupportedMarket);
    EXPECT_EQ(builder.account_overview(MarketType::Option, k_signed_at).error(),
              BuildError::UnsupportedMarket);
    EXPECT_EQ(builder.bullet_private(MarketType::Option, k_signed_at).error(),
              BuildError::UnsupportedMarket);
    EXPECT_TRUE(private_order_topic(MarketType::Option).empty());
}

// ---------------------------------------------------- private stream ----

TEST(KucoinWirePrivate, ThePrivateTopicCarriesNoSymbolBehindIt) {
    // The public discriminator reads the symbol out of the topic and so
    // refuses these frames outright; the payload names it instead.
    for (const auto name : {"ws_spot_order_open.json", "ws_futures_order_match.json"}) {
        const auto frame = fixture(name);
        EXPECT_FALSE(parse_event(frame).has_value()) << name;
        const auto event = parse_private_event(frame);
        ASSERT_TRUE(event.has_value()) << name;
        EXPECT_EQ(event->kind, EventKind::OrderChange) << name;
    }
    // The per-symbol form of the futures topic renames the subject and keeps
    // the payload; reading only `orderChange` would silently drop every frame.
    const auto per_symbol = fixture("ws_futures_order_symbol_match.json");
    ASSERT_TRUE(parse_private_event(per_symbol).has_value());
    EXPECT_EQ(parse_private_event(per_symbol)->kind, EventKind::OrderChange);

    // Control frames still arrive on this socket and are not order changes.
    EXPECT_EQ(parse_private_event(fixture("ws_welcome.json"))->kind, EventKind::Welcome);
    EXPECT_EQ(parse_private_event(fixture("ws_error_token.json"))->kind, EventKind::Error);
}

TEST(KucoinWirePrivate, ParsedViewsPointIntoTheCallerBuffer) {
    // The private path makes the same zero-copy promise the public one does,
    // and breaks louder: every field below is read AFTER the frame is handled,
    // by a mapper that resolves the symbol against the instrument table.
    const auto frame = fixture("ws_futures_order_match.json");
    const auto change = parse_order_change(frame);
    ASSERT_TRUE(change.has_value());
    const auto* begin = frame.data();
    const auto* end = begin + frame.size();
    for (const auto view : {change->symbol,
                            change->order_id,
                            change->client_oid,
                            change->trade_id,
                            change->match_price,
                            change->match_size}) {
        EXPECT_GE(view.data(), begin);
        EXPECT_LE(view.data() + view.size(), end);
    }

    static FillBuffer storage;
    const auto body = fixture("rest_futures_fills.json");
    const auto fills = parse_fills(body, storage);
    ASSERT_TRUE(fills.has_value());
    ASSERT_EQ(fills->size(), 1U);
    const auto* body_begin = body.data();
    const auto* body_end = body_begin + body.size();
    const auto& fill = (*fills)[0];
    for (const auto view : {fill.symbol, fill.trade_id, fill.order_id, fill.price, fill.size}) {
        EXPECT_GE(view.data(), body_begin);
        EXPECT_LE(view.data() + view.size(), body_end);
    }
}

TEST(KucoinWirePrivate, OrderTimeMeansMillisecondsOnSpotAndNanosecondsOnFutures) {
    // Same field name, two scales, six orders of magnitude apart. `ts` is
    // nanoseconds on both, which is why it is the one events are timed by.
    const auto spot_frame = fixture("ws_spot_order_open.json");
    const auto spot = parse_order_change(spot_frame);
    ASSERT_TRUE(spot.has_value());
    EXPECT_EQ(spot->order_time, 1730211033305ULL);
    EXPECT_EQ(spot->ts_ns, 1730211033335000000ULL);

    const auto futures_frame = fixture("ws_futures_order_match.json");
    const auto futures = parse_order_change(futures_frame);
    ASSERT_TRUE(futures.has_value());
    // Quoted on futures, bare on spot; both reach the DTO as digits.
    EXPECT_EQ(futures->order_time, 1772043995356345762ULL);
    EXPECT_EQ(futures->ts_ns, 1772043995362000000ULL);
}

TEST(KucoinMapPrivate, AnAckBecomesAcceptedAndCarriesWhatIsStillWorking) {
    const auto table = loaded_spot_table();
    const auto frame = fixture("ws_spot_order_open.json");
    const auto wire = parse_order_change(frame);
    ASSERT_TRUE(wire.has_value());
    const auto event = map_order_change(*wire, table, UnixNanos{5});
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::Accepted);
    EXPECT_EQ(event->side, Side::Buy);
    EXPECT_EQ(event->order_type, OrderType::Limit);
    EXPECT_EQ(event->exchange_client_order_id.view(), "01J8ZQK9V7M3XPB2R6TDNAWC5E");
    EXPECT_EQ(event->exchange_order_id.view(), "6720ecd9ec71f4000747731a");
    EXPECT_EQ(event->price.raw, 500'000); // 50000 over the 0.1 tick
    EXPECT_EQ(event->qty.raw, 1'000);     // 0.00001 over the 0.00000001 lot
    EXPECT_EQ(event->leaves_qty.raw, 1'000);
    EXPECT_EQ(event->ts_event.ns, 1730211033335000000ULL);
    EXPECT_EQ(event->ts_init.ns, 5U);
}

TEST(KucoinMapPrivate, APartialFillIsTheMatchPriceAndWhatIsLeftAfterIt) {
    const auto table = loaded_spot_table();
    const auto frame = fixture("ws_spot_order_match.json");
    const auto wire = parse_order_change(frame);
    ASSERT_TRUE(wire.has_value());
    const auto event = map_order_change(*wire, table, UnixNanos{5});
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::Filled);
    // The FILL's price and size, not the order's: an order resting at 50000
    // and a fill at 50000 are the same number here only by coincidence.
    EXPECT_EQ(event->qty.raw, 400);        // 0.000004 traded
    EXPECT_EQ(event->leaves_qty.raw, 600); // 0.000006 still working
    EXPECT_EQ(event->price.raw, 500'000);
    EXPECT_EQ(event->liquidity, Liquidity::Taker);
    EXPECT_EQ(event->exec_id.view(), "11029373945659392");
    // The frame names the fee TYPE and never the amount, so this is what the
    // ledger can do arithmetic on — an undef would poison it.
    EXPECT_EQ(event->fee.raw, 0);
}

TEST(KucoinMapPrivate, AFullFillOnFuturesIsPricedInContracts) {
    const auto table = loaded_table();
    const auto frame = fixture("ws_futures_order_match.json");
    const auto wire = parse_order_change(frame);
    ASSERT_TRUE(wire.has_value());
    const auto event = map_order_change(*wire, table, UnixNanos{9});
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::Filled);
    EXPECT_EQ(event->order_type, OrderType::Market);
    // A market order's `price` is "0"; only matchPrice says what traded.
    EXPECT_EQ(event->price.raw, 206'855); // 2068.55 over the 0.01 tick
    EXPECT_EQ(event->qty.raw, 1);
    EXPECT_EQ(event->leaves_qty.raw, 0);
    EXPECT_EQ(event->liquidity, Liquidity::Taker);
    EXPECT_EQ(event->exec_id.view(), "1815302608109");
}

TEST(KucoinMapPrivate, TheFilledNoticeIsNotASecondFill) {
    // KuCoin sends one `match` per trade and then a `filled` closing the
    // order. Emitting both counts the last trade twice, which shows up as a
    // position we never took.
    const auto table = loaded_table();
    const auto frame = fixture("ws_futures_order_filled.json");
    const auto wire = parse_order_change(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->type, OrderChangeType::Filled);
    EXPECT_FALSE(map_order_change(*wire, table, UnixNanos{9}).has_value());
}

TEST(KucoinMapPrivate, ACancelIsTerminalAndKeepsItsIdentity) {
    const auto table = loaded_table();
    const auto frame = fixture("ws_futures_order_canceled.json");
    const auto wire = parse_order_change(frame);
    ASSERT_TRUE(wire.has_value());
    const auto event = map_order_change(*wire, table, UnixNanos{9});
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::Canceled);
    EXPECT_EQ(event->side, Side::Sell);
    EXPECT_EQ(event->qty.raw, 3);
    EXPECT_EQ(event->leaves_qty.raw, 0);
    EXPECT_EQ(event->exchange_client_order_id.view(), "01J8ZQK9V7M3XPB2R6TDNAWC5E");
}

TEST(KucoinMapPrivate, FramesThatCannotBeAttributedOrScaledAreDropped) {
    const auto table = loaded_table();
    const auto frame = fixture("ws_futures_order_match.json");
    auto wire = parse_order_change(frame).value();

    // A clientOid longer than a ULID cannot be one of ours — it belongs to
    // another client of the same account — and matching it to a truncated
    // lookalike is worse than not hearing it.
    wire.client_oid = "9f7a2be0-effe-45bd-bdc8-1614715a583a";
    EXPECT_FALSE(map_order_change(wire, table, UnixNanos{1}).has_value());

    wire = parse_order_change(frame).value();
    wire.symbol = "NOSUCHSYM";
    EXPECT_FALSE(map_order_change(wire, table, UnixNanos{1}).has_value());

    // Half a tick. Rounding it would report a fill at a price that never
    // traded, and the ledger would carry the difference forever.
    wire = parse_order_change(frame).value();
    wire.match_price = "2068.555";
    EXPECT_FALSE(map_order_change(wire, table, UnixNanos{1}).has_value());

    // A fraction of a contract is refused the same way.
    wire = parse_order_change(frame).value();
    wire.match_size = "0.5";
    EXPECT_FALSE(map_order_change(wire, table, UnixNanos{1}).has_value());
}

TEST(KucoinWs, ThePrivateSubscribeFrameSaysSoOutLoud) {
    Protocol protocol;
    EXPECT_EQ(private_order_topic(MarketType::Spot), "/spotMarket/tradeOrders");
    EXPECT_EQ(private_order_topic(MarketType::Perp), "/contractMarket/tradeOrders");
    EXPECT_EQ(private_order_topic(MarketType::Future), "/contractMarket/tradeOrders");

    const auto frame = protocol.private_subscribe_frame(private_order_topic(MarketType::Perp));
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(*frame,
              R"({"id":1,"type":"subscribe","topic":"/contractMarket/tradeOrders",)"
              R"("privateChannel":true,"response":true})");
    // No symbol list: the topic covers the whole account.
    EXPECT_EQ(frame->find(':' + std::string{"XBTUSDTM"}), std::string_view::npos);
    EXPECT_FALSE(protocol.private_subscribe_frame({}).has_value());
}

// ------------------------------------------------ reconciliation pulls ----

TEST(KucoinMapRest, TheSpotSweepReadsAnUnwrappedArray) {
    const auto table = loaded_spot_table();
    static OrderBuffer storage;
    const auto body = fixture("rest_spot_orders_active.json");
    const auto wire = parse_spot_orders(body, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->size(), 2U);

    const auto resting = map_open_order((*wire)[0], table);
    ASSERT_TRUE(resting.has_value());
    EXPECT_EQ(resting->status, OrderStatus::Accepted);
    EXPECT_EQ(resting->qty.raw, 1'000);
    EXPECT_EQ(resting->leaves.raw, 1'000);
    EXPECT_EQ(resting->price.raw, 500'000);
    EXPECT_EQ(resting->exchange_client_order_id.view(), "01J8ZQK9V7M3XPB2R6TDNAWC5E");
    EXPECT_EQ(resting->ts_venue.ns, 1729235909862ULL * 1'000'000ULL);

    const auto working = map_open_order((*wire)[1], table);
    ASSERT_TRUE(working.has_value());
    EXPECT_EQ(working->status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(working->side, Side::Sell);
    EXPECT_EQ(working->leaves.raw, 600); // spot states what is left
}

TEST(KucoinMapRest, TheFuturesSweepReadsAPageAndSubtractsWhatIsLeft) {
    const auto table = loaded_table();
    static OrderBuffer storage;
    const auto body = fixture("rest_futures_orders_active.json");
    // The same rows under `data` directly would be a spot answer; futures
    // wraps them in a page object, and reading one as the other yields zero
    // open orders, which a reconciler believes.
    EXPECT_FALSE(parse_spot_orders(body, storage).has_value());
    const auto wire = parse_futures_orders(body, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->size(), 2U);

    const auto resting = map_open_order((*wire)[0], table);
    ASSERT_TRUE(resting.has_value());
    EXPECT_EQ(resting->status, OrderStatus::Accepted);
    EXPECT_EQ(resting->qty.raw, 3);
    EXPECT_EQ(resting->leaves.raw, 3);
    EXPECT_EQ(resting->price.raw, 641'256);

    // Futures never states remainSize, so what is left is size minus dealSize.
    const auto working = map_open_order((*wire)[1], table);
    ASSERT_TRUE(working.has_value());
    EXPECT_EQ(working->status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(working->qty.raw, 5);
    EXPECT_EQ(working->leaves.raw, 3);
}

TEST(KucoinMapRest, ActiveSymbolsIsTheFirstHalfOfTheSpotSweep) {
    static SymbolBuffer storage;
    const auto body = fixture("rest_spot_active_symbols.json");
    const auto symbols = parse_active_symbols(body, storage);
    ASSERT_TRUE(symbols.has_value());
    ASSERT_EQ(symbols->size(), 1U);
    EXPECT_EQ((*symbols)[0], "BTC-USDT");
}

TEST(KucoinMapRest, FillsShareOneShapeAcrossTwoHosts) {
    static FillBuffer storage;
    const auto futures_table = loaded_table();
    const auto futures_body = fixture("rest_futures_fills.json");
    const auto futures = parse_fills(futures_body, storage);
    ASSERT_TRUE(futures.has_value());
    ASSERT_EQ(futures->size(), 1U);
    const auto perp = map_fill((*futures)[0], futures_table);
    ASSERT_TRUE(perp.has_value());
    EXPECT_EQ(perp->side, Side::Sell);
    EXPECT_EQ(perp->liquidity, Liquidity::Taker);
    EXPECT_EQ(perp->price.raw, 641'400);
    EXPECT_EQ(perp->qty.raw, 2);
    EXPECT_EQ(perp->fee.raw, 76'900'000);
    EXPECT_EQ(perp->exec_id.view(), "1940359085512");
    EXPECT_EQ(perp->exchange_order_id.view(), "230181737576050688");
    // Neither host names the clientOid on a fill; the order id is the link.
    EXPECT_TRUE(perp->exchange_client_order_id.view().empty());
    EXPECT_EQ(perp->ts_venue.ns, 1727692805000ULL * 1'000'000ULL);

    const auto spot_table = loaded_spot_table();
    const auto spot_body = fixture("rest_spot_fills.json");
    const auto spot = parse_fills(spot_body, storage);
    ASSERT_TRUE(spot.has_value());
    ASSERT_EQ(spot->size(), 1U);
    // tradeId is a JSON number here and a quoted string on futures.
    EXPECT_EQ((*spot)[0].trade_id, "11029373945659392");
    const auto fill = map_fill((*spot)[0], spot_table);
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->price.raw, 677'176);
    EXPECT_EQ(fill->qty.raw, 1'000);
    EXPECT_EQ(fill->fee.raw, 677'176);
}

TEST(KucoinMapRest, APositionKeepsItsSignAndSurvivesAnAverageOffTheTick) {
    const auto table = loaded_table();
    static PositionBuffer storage;
    const auto body = fixture("rest_positions.json");
    const auto wire = parse_positions(body, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->size(), 2U);

    const auto long_side = map_position((*wire)[0], table);
    ASSERT_TRUE(long_side.has_value());
    EXPECT_EQ(long_side->net_qty.raw, 1);
    EXPECT_EQ(long_side->avg_entry.raw, 1'200'164); // 120016.4 over the 0.1 tick
    EXPECT_EQ(long_side->ts_venue.ns, 1758507727807ULL * 1'000'000ULL);

    // currentQty is signed; there is no side field that could disagree.
    const auto short_side = map_position((*wire)[1], table);
    ASSERT_TRUE(short_side.has_value());
    EXPECT_EQ(short_side->net_qty.raw, -40);
    // 2068.555 is not a multiple of the 0.01 tick, which is normal for an
    // average of fills. Discarding the row over it would lose the quantity —
    // the only part reconciliation acts on.
    EXPECT_TRUE(short_side->avg_entry.is_undef());
}

TEST(KucoinRest, TheWalletIsAskedForOneNamedCurrency) {
    Credentials credentials;
    load_test_credentials(credentials);
    OrderRequestBuilder builder{credentials};

    const auto request = builder.account_overview(MarketType::Perp, k_signed_at);
    ASSERT_TRUE(request.has_value());
    // Omitting the currency reads the XBT wallet, which is an answer about an
    // account this gateway does not trade rather than an error.
    EXPECT_EQ(request->target, "/api/v1/account-overview?currency=USDT");
    EXPECT_TRUE(request->body.empty());
    EXPECT_EQ(builder.last_sign_message(),
              "1730211033335GET/api/v1/account-overview?currency=USDT");
    // The spot wallet is a different endpoint on a different host.
    EXPECT_EQ(builder.account_overview(MarketType::Spot, k_signed_at).error(),
              BuildError::UnsupportedRequest);
}

TEST(KucoinMapRest, TheWalletIsMarginBalanceAndNeverTheEquityBesideIt) {
    const auto body = fixture("rest_account_overview.json");
    const auto wire = parse_account_overview(body);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->currency, "USDT");

    constexpr UnixNanos k_read_at{1'730'211'033'335'000'000ULL};
    const auto balance = map_account_overview(*wire, k_read_at);
    ASSERT_TRUE(balance.has_value());
    EXPECT_EQ(balance->asset.view(), "USDT");
    // marginBalance 100.2, and NOT accountEquity 99.8999305281: KuCoin's
    // equity already contains the pnl carried on the field below, so a caller
    // adding the two would count it twice.
    EXPECT_EQ(balance->total.raw, 100'200'000'000);
    EXPECT_NE(balance->total.raw, 99'899'930'528);
    EXPECT_EQ(balance->available.raw, 50'100'000'000);
    // -0.3000694719 is ten fractional digits; the exact reader refuses the
    // whole number, which would show as an account holding nothing.
    EXPECT_FALSE(parse_decimal_e9("-0.3000694719").has_value());
    EXPECT_EQ(balance->unrealized.raw, -300'069'471);
    // The overview states no clock of its own.
    EXPECT_EQ(balance->ts_venue.ns, k_read_at.ns);
}

TEST(KucoinMapRest, AWalletFigureKucoinOmitsStaysUndefRatherThanZero) {
    // Undef and zero are different facts, and only one of them means the money
    // is gone. This body names the currency and nothing else.
    const std::string body = R"({"code":"200000","data":{"currency":"USDT"}})";
    const auto wire = parse_account_overview(body);
    ASSERT_TRUE(wire.has_value());

    const auto balance = map_account_overview(*wire, UnixNanos{7});
    ASSERT_TRUE(balance.has_value());
    EXPECT_TRUE(balance->total.is_undef());
    EXPECT_TRUE(balance->available.is_undef());
    EXPECT_TRUE(balance->unrealized.is_undef());

    // A wallet with no currency is a number with no unit; there is no row to
    // report, and reporting one under an empty asset would be worse.
    const std::string unnamed = R"({"code":"200000","data":{"marginBalance":100.2}})";
    EXPECT_FALSE(parse_account_overview(unnamed).has_value());
}

TEST(KucoinMapRest, TheSpotCatalogueAdmitsOnlyTradablePairsItCanScale) {
    static SpotSymbolBuffer storage;
    const auto body = fixture("rest_spot_symbols.json");
    const auto wire = parse_spot_symbols(body, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->size(), 4U);

    const auto btc = map_spot_symbol((*wire)[0]);
    ASSERT_TRUE(btc.has_value());
    ASSERT_TRUE(std::holds_alternative<SpotInstrument>(*btc));
    const auto& base = base_of(*btc);
    EXPECT_EQ(base.exchange_symbol.view(), "BTC-USDT");
    EXPECT_EQ(base.tick_size.raw, 100'000'000); // 0.1
    EXPECT_EQ(base.lot_size.raw, 10);           // 0.00000001
    EXPECT_EQ(base.min_notional.raw, 100'000'000);
    // A spot lot IS one base unit; there is no contract in between.
    EXPECT_EQ(base.contract_value.raw, k_e9);
    EXPECT_EQ(base.id.market(), MarketType::Spot);

    EXPECT_TRUE(map_spot_symbol((*wire)[1]).has_value()); // ETH-USDT
    // enableTrading false: the only suspended pair of 1003 on the capture date.
    EXPECT_FALSE((*wire)[2].enable_trading);
    EXPECT_FALSE(map_spot_symbol((*wire)[2]).has_value());
    // priceIncrement 1e-12, three decades below what FixedE9 models. Refusing
    // beats rounding a tick size to zero.
    EXPECT_EQ((*wire)[3].price_increment, "0.000000000001");
    EXPECT_FALSE(map_spot_symbol((*wire)[3]).has_value());
}

TEST(KucoinErrors, A429HoldsForTheWindowKucoinStates) {
    // Headers as api-futures.kucoin.com sent them with a 429 on 2026-09-26.
    const std::array<net::RateHeader, 3> refused{{{"gw-ratelimit-remaining", "0"},
                                                  {"gw-ratelimit-limit", "2000"},
                                                  {"gw-ratelimit-reset", "17019"}}};
    EXPECT_EQ(refusal_hold_ms(refused), 17019U);
    const std::array<net::RateHeader, 1> upper{{{"Gw-RateLimit-Reset", "250"}}};
    EXPECT_EQ(refusal_hold_ms(upper), 250U);
    const std::array<net::RateHeader, 1> runaway{{{"gw-ratelimit-reset", "999999"}}};
    EXPECT_EQ(refusal_hold_ms(runaway), 30000U);
    EXPECT_EQ(refusal_hold_ms({}), 1000U);
}

TEST(KucoinErrors, ACancelThatLostItsRaceIsNotACancelReject) {
    // The order is already gone, which is what the cancel asked for. Reporting
    // CancelRejected would resurrect an order the OMS has written off.
    const auto spot_body = fixture("rest_error_cancel_gone_spot.json");
    const auto spot = parse_rest_envelope(spot_body);
    ASSERT_TRUE(spot.has_value());
    EXPECT_EQ(spot->code, k_parameter_error);
    EXPECT_TRUE(is_benign_cancel_failure(spot->code, spot->msg));

    const auto futures_body = fixture("rest_error_cancel_gone_futures.json");
    const auto futures = parse_rest_envelope(futures_body);
    ASSERT_TRUE(futures.has_value());
    EXPECT_EQ(futures->code, k_futures_order_not_cancelable);
    EXPECT_TRUE(is_benign_cancel_failure(futures->code, futures->msg));

    // 400100 is the catch-all parameter error, so the code alone must not
    // swallow a malformed cancel as harmless.
    EXPECT_FALSE(
        is_benign_cancel_failure(k_parameter_error, "pageSize should not greater than 500"));
    EXPECT_FALSE(is_benign_cancel_failure(k_spot_balance_insufficient, "Balance insufficient"));
    // A success body is not a failure of any kind.
    EXPECT_TRUE(parse_rest_envelope(fixture("rest_order_ack.json"))->code == k_ok);
}

TEST(KucoinErrors, OrderEntryRefusalsLandInTheBucketTheGatewayActsOn) {
    // Two separate tables: KuCoin numbers spot and futures refusals
    // independently, and neither is a superset of the other.
    EXPECT_EQ(classify(k_spot_balance_insufficient), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_spot_order_count_exceeded), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_spot_symbol_not_tradable), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_spot_symbol_unknown), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_futures_balance_insufficient), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_futures_client_oid_repeated), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_futures_leverage_too_high), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_futures_no_position_to_close), exec::ErrorClass::Terminal);
    // A network timeout is the venue saying nothing, not the venue saying no.
    EXPECT_EQ(classify(k_network_timeout), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_order_forbidden), exec::ErrorClass::Terminal);
}

// ------------------------------------------------- spot market data ----

TEST(KucoinSpotWire, TheSpotQuoteRenamesEveryFieldAndChangesTheClock) {
    // Futures: bestBidPrice and a NANOSECOND ts. Spot: bestBid and a
    // MILLISECOND time. Read the futures way, the spot quote has no prices at
    // all — which is a refusal, not a wrong number, and that is the only
    // reason this was ever safe to get wrong.
    const auto frame = fixture("ws_spot_ticker.json");
    EXPECT_FALSE(parse_bbo(frame, MarketType::Perp).has_value());

    const auto wire = parse_bbo(frame, MarketType::Spot);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "BTC-USDT");
    EXPECT_FALSE(wire->bid_price.empty());
    EXPECT_FALSE(wire->ask_price.empty());
    // Milliseconds scaled to the nanoseconds every other KuCoin stamp is in.
    EXPECT_GT(wire->ts_ns, 1'000'000'000'000'000'000ULL);
}

TEST(KucoinSpotWire, TheSpotPrintStampsNanosecondsAsAString) {
    const auto frame = fixture("ws_spot_match.json");
    const auto wire = parse_trade(frame, MarketType::Spot);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "BTC-USDT");
    EXPECT_FALSE(wire->trade_id.empty());
    EXPECT_GT(wire->ts_ns, 1'000'000'000'000'000'000ULL);
    EXPECT_EQ(wire->sequence, 0) << "the spot print carries no sequence";
}

TEST(KucoinSpotWire, TheSpotDepthTopicIsAWholeBook) {
    // Both topics carry the subject "level2": one packs a single level change
    // into a string, the other sends the whole book. Only the socket knows.
    auto storage = std::make_unique<LevelBuffer>();
    const auto frame = fixture("ws_spot_level2depth.json");
    const auto wire = parse_book_snapshot(frame, *storage);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "BTC-USDT");
    ASSERT_FALSE(wire->bids.empty());
    ASSERT_FALSE(wire->asks.empty());

    const auto table = loaded_spot_table();
    const auto book = map_book(*wire, table, UnixNanos{3});
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->instrument.market(), MarketType::Spot);
    EXPECT_GT(book->bid_count, 0U);
    EXPECT_LT(book->bids[0].price.raw, book->asks[0].price.raw) << "an uncrossed book";
}

// UTA v2's `obu` channel. Every frame below was captured from
// wss://x-push-futures.kucoin.com on 2026-09-20 with no credentials of any
// kind — the endpoint takes public market data without a token, which is what
// makes this migration a parser change rather than an account migration.

TEST(KucoinUtaBook, ASnapshotOpensTheStreamAndNamesOneSequence) {
    const auto frame = fixture("ws_uta_book_snapshot.json");
    auto storage = std::make_unique<LevelBuffer>();
    const auto wire = parse_uta_book(frame, *storage);
    ASSERT_TRUE(wire.has_value());
    EXPECT_TRUE(wire->is_snapshot);
    EXPECT_EQ(wire->symbol, "XBTUSDTM");
    // "For Snapshot pushes the values of O and C are always the same" — it is
    // the sequence already inside these levels, the same convention the REST
    // seed used before this channel existed.
    EXPECT_EQ(wire->seq_start, wire->seq_end);
    ASSERT_FALSE(wire->bids.empty());
    ASSERT_FALSE(wire->asks.empty());
    EXPECT_GT(wire->ts_ns, 1'700'000'000'000'000'000ULL) << "nanoseconds, not the classic ms";
}

TEST(KucoinUtaBook, ADeltaCarriesTheSpanItCovers) {
    const auto frame = fixture("ws_uta_book_delta.json");
    auto storage = std::make_unique<LevelBuffer>();
    const auto wire = parse_uta_book(frame, *storage);
    ASSERT_TRUE(wire.has_value());
    EXPECT_FALSE(wire->is_snapshot);
    EXPECT_EQ(wire->symbol, "XBTUSDTM");
    // ONE FRAME, MANY LEVELS, so it names a range rather than a number. This
    // is the part that does not port from classic, where a reader requires
    // sequence == last + 1.
    EXPECT_GT(wire->seq_end, wire->seq_start);
}

TEST(KucoinUtaBook, BothSidesAreRequired) {
    auto storage = std::make_unique<LevelBuffer>();
    const auto wire = parse_uta_book(fixture("ws_uta_book_delta_next.json"), *storage);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->asks.size(), 4U);
    EXPECT_EQ(wire->bids.size(), 3U);

    // AN UNTOUCHED SIDE ARRIVES AS [], NEVER ABSENT, so a missing key is a
    // truncated frame rather than "nothing changed" — reading it as the latter
    // would apply half a book and call it applied.
    EXPECT_FALSE(
        parse_uta_book(R"({"t":"delta","d":{"s":"X","O":1,"C":2,"a":[["1","1"]]}})", *storage)
            .has_value())
        << "no bids key at all";
    const auto empty_side = parse_uta_book(
        R"({"t":"delta","d":{"s":"X","O":1,"C":2,"a":[],"b":[["1","1"]]}})", *storage);
    ASSERT_TRUE(empty_side.has_value()) << "an empty side is a side";
    EXPECT_TRUE(empty_side->asks.empty());
    EXPECT_EQ(empty_side->bids.size(), 1U);
}

TEST(KucoinUtaBook, AFrameWithoutItsSpanIsRefused) {
    auto storage = std::make_unique<LevelBuffer>();
    // A frame that cannot be chained must not be applied: without O and C
    // there is no way to tell a replay from a gap.
    EXPECT_FALSE(
        parse_uta_book(R"({"t":"delta","d":{"s":"X","C":2,"a":[],"b":[]}})", *storage).has_value());
    EXPECT_FALSE(
        parse_uta_book(R"({"t":"delta","d":{"s":"X","O":1,"a":[],"b":[]}})", *storage).has_value());
}

TEST(KucoinUtaBook, AFrameThatIsNeitherSnapshotNorDeltaIsRefused) {
    auto storage = std::make_unique<LevelBuffer>();
    EXPECT_FALSE(parse_uta_book(fixture("ws_uta_subscribe_ack.json"), *storage).has_value())
        << "a subscribe acknowledgement is not a book";
    EXPECT_FALSE(parse_uta_book(R"({"t":"heartbeat","d":{}})", *storage).has_value());
}

// The chaining rule, which is the half of this migration that cannot be read
// off a frame. Classic requires `sequence == last + 1`; here one frame covers
// a span and the venue's rule is start <= last_end + 1 and end > last_end.

namespace {

WireUtaBook uta(std::string_view symbol,
                bool snapshot,
                std::int64_t start,
                std::int64_t end,
                std::span<const WireLevel> bids,
                std::span<const WireLevel> asks) {
    WireUtaBook out{};
    out.symbol = symbol;
    out.is_snapshot = snapshot;
    out.seq_start = start;
    out.seq_end = end;
    out.ts_ns = 1'789'000'000'000'000'000ULL;
    out.bids = bids;
    out.asks = asks;
    return out;
}

} // namespace

TEST(KucoinUtaChain, ADeltaBeforeTheSnapshotIsNotApplied) {
    UtaBook book{InstrumentId{1}, FixedE9::from_raw(k_e9 / 10), FixedE9::from_raw(k_e9)};
    const std::array<WireLevel, 1> bid{WireLevel{.price = "100.0", .qty = "5"}};
    const std::array<WireLevel, 1> ask{WireLevel{.price = "101.0", .qty = "7"}};
    EXPECT_EQ(book.apply(uta("X", false, 10, 12, bid, ask)), BookResult::Waiting);
    EXPECT_FALSE(book.live());
    EXPECT_FALSE(book.snapshot(UnixNanos{1}, UnixNanos{2}).has_value());
}

TEST(KucoinUtaChain, ASnapshotOpensTheBookAndDeltasCarryItForward) {
    UtaBook book{InstrumentId{1}, FixedE9::from_raw(k_e9 / 10), FixedE9::from_raw(k_e9)};
    const std::array<WireLevel, 1> bid{WireLevel{.price = "100.0", .qty = "5"}};
    const std::array<WireLevel, 1> ask{WireLevel{.price = "101.0", .qty = "7"}};
    ASSERT_EQ(book.apply(uta("X", true, 500, 500, bid, ask)), BookResult::Applied);
    EXPECT_TRUE(book.live());
    EXPECT_EQ(book.last_sequence(), 500);

    // Contiguous: starts exactly where the last one ended, plus one.
    const std::array<WireLevel, 1> better{WireLevel{.price = "100.5", .qty = "9"}};
    EXPECT_EQ(book.apply(uta("X", false, 501, 503, better, {})), BookResult::Applied);
    EXPECT_EQ(book.last_sequence(), 503);

    // OVERLAPPING IS LEGAL AND A GAP IS NOT, which is the whole difference
    // from classic: a frame may restate sequences already applied as long as
    // it also carries new ones.
    EXPECT_EQ(book.apply(uta("X", false, 502, 505, {}, {})), BookResult::Applied);
    EXPECT_EQ(book.last_sequence(), 505);

    const auto top = book.bbo(UnixNanos{1}, UnixNanos{2});
    ASSERT_TRUE(top.has_value());
    EXPECT_LT(top->bid.raw, top->ask.raw) << "an uncrossed book";
}

TEST(KucoinUtaChain, AReplayIsIgnoredAndAGapDropsTheBook) {
    UtaBook book{InstrumentId{1}, FixedE9::from_raw(k_e9 / 10), FixedE9::from_raw(k_e9)};
    const std::array<WireLevel, 1> bid{WireLevel{.price = "100.0", .qty = "5"}};
    const std::array<WireLevel, 1> ask{WireLevel{.price = "101.0", .qty = "7"}};
    ASSERT_EQ(book.apply(uta("X", true, 500, 500, bid, ask)), BookResult::Applied);

    // end <= last_end: everything in it is already inside the book.
    EXPECT_EQ(book.apply(uta("X", false, 498, 500, {}, {})), BookResult::Stale);
    EXPECT_EQ(book.last_sequence(), 500) << "a replay must not move the chain";
    EXPECT_TRUE(book.live());

    // start > last_end + 1: something was skipped, and a book that applied it
    // would disagree with KuCoin's permanently and invisibly.
    EXPECT_EQ(book.apply(uta("X", false, 502, 504, {}, {})), BookResult::Gap);
    EXPECT_FALSE(book.live());
    EXPECT_EQ(book.gaps(), 1U);
    EXPECT_FALSE(book.snapshot(UnixNanos{1}, UnixNanos{2}).has_value())
        << "a dropped book publishes nothing at all";

    // NO SEED IS FETCHED. The venue resends a snapshot on this same
    // subscription, so recovery is waiting rather than asking.
    ASSERT_EQ(book.apply(uta("X", true, 900, 900, bid, ask)), BookResult::Applied);
    EXPECT_TRUE(book.live());
    EXPECT_EQ(book.last_sequence(), 900);
}

TEST(KucoinUtaChain, ASnapshotReplacesRatherThanMerges) {
    UtaBook book{InstrumentId{1}, FixedE9::from_raw(k_e9 / 10), FixedE9::from_raw(k_e9)};
    const std::array<WireLevel, 2> bids{WireLevel{.price = "100.0", .qty = "5"},
                                        WireLevel{.price = "99.0", .qty = "3"}};
    const std::array<WireLevel, 1> ask{WireLevel{.price = "101.0", .qty = "7"}};
    ASSERT_EQ(book.apply(uta("X", true, 1, 1, bids, ask)), BookResult::Applied);
    const auto first = book.snapshot(UnixNanos{1}, UnixNanos{2});
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->bid_count, 2U);

    // A restatement the venue sends after its own break must not leave behind
    // a level it has dropped.
    const std::array<WireLevel, 1> fewer{WireLevel{.price = "100.0", .qty = "5"}};
    ASSERT_EQ(book.apply(uta("X", true, 9, 9, fewer, ask)), BookResult::Applied);
    const auto second = book.snapshot(UnixNanos{1}, UnixNanos{2});
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->bid_count, 1U) << "99.0 was not restated and must be gone";
}

TEST(KucoinUtaChain, ASizeOfZeroRemovesALevel) {
    UtaBook book{InstrumentId{1}, FixedE9::from_raw(k_e9 / 10), FixedE9::from_raw(k_e9)};
    const std::array<WireLevel, 2> bids{WireLevel{.price = "100.0", .qty = "5"},
                                        WireLevel{.price = "99.0", .qty = "3"}};
    const std::array<WireLevel, 1> ask{WireLevel{.price = "101.0", .qty = "7"}};
    ASSERT_EQ(book.apply(uta("X", true, 1, 1, bids, ask)), BookResult::Applied);
    const std::array<WireLevel, 1> gone{WireLevel{.price = "99.0", .qty = "0"}};
    ASSERT_EQ(book.apply(uta("X", false, 2, 2, gone, {})), BookResult::Applied);
    const auto after = book.snapshot(UnixNanos{1}, UnixNanos{2});
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->bid_count, 1U);
}

TEST(KucoinUtaWs, ASubscribeAsksForOneSymbolAtATime) {
    Protocol protocol;
    const std::array<Subscription, 2> subs{
        Subscription{.topic = k_uta_book_channel, .symbol = "XBTUSDTM"},
        Subscription{.topic = k_uta_book_channel, .symbol = "ETHUSDTM"}};
    const auto first = protocol.uta_subscribe_frame(subs);
    ASSERT_TRUE(first.has_value());
    // NEVER BATCHED: `symbols` as an array is refused for this channel with
    // "symbols are not supported for topic obu", so a catalogue costs a frame
    // per contract and the send spacing is what keeps it legal.
    EXPECT_EQ(first->consumed, 1U);
    EXPECT_EQ(first->text,
              R"({"id":"1","action":"SUBSCRIBE","channel":"obu","tradeType":"FUTURES",)"
              R"("symbol":"XBTUSDTM","depth":"increment@10ms"})");

    const auto second = protocol.uta_subscribe_frame(std::span{subs}.subspan(1));
    ASSERT_TRUE(second.has_value());
    EXPECT_NE(second->text.find("ETHUSDTM"), std::string_view::npos);
    EXPECT_NE(second->text.find(R"("id":"2")"), std::string_view::npos) << "ids advance";

    protocol.set_uta_book_depth("50");
    const auto third = protocol.uta_subscribe_frame(subs);
    ASSERT_TRUE(third.has_value());
    EXPECT_NE(third->text.find(R"("depth":"50"})"), std::string_view::npos);
}

TEST(KucoinUtaWs, TradesSubscribeOnTheTradeChannelWithoutADepth) {
    EXPECT_EQ(uta_topic_for(exec::SubKind::Trades), k_uta_trade_channel);
    Protocol protocol;
    const std::array<Subscription, 1> subs{
        Subscription{.topic = k_uta_trade_channel, .symbol = "XBTUSDTM"}};
    const auto frame = protocol.uta_subscribe_frame(subs);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->text,
              R"({"id":"1","action":"SUBSCRIBE","channel":"trade","tradeType":"FUTURES",)"
              R"("symbol":"XBTUSDTM"})");
}

TEST(KucoinUtaTrade, APrintParsesAndABookFrameDoesNot) {
    const auto frame = fixture("ws_uta_trade.json");
    const auto wire = parse_uta_trade(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "XBTUSDTM");
    EXPECT_EQ(wire->price, "83929.4");
    EXPECT_EQ(wire->qty, "1");
    EXPECT_FALSE(wire->aggressor_bought);
    EXPECT_EQ(wire->trade_id, "1945076528422");
    EXPECT_EQ(wire->sequence, 1945076528422);
    EXPECT_EQ(wire->ts_ns, 1790432266600000000ULL);
    EXPECT_FALSE(parse_uta_trade(fixture("ws_uta_book_snapshot.json")).has_value());
    EXPECT_FALSE(parse_uta_trade(fixture("ws_uta_subscribe_ack.json")).has_value());
}

TEST(KucoinUtaWs, ARequestForAnotherChannelIsRefused) {
    Protocol protocol;
    const std::array<Subscription, 1> classic{
        Subscription{.topic = "/contractMarket/level2", .symbol = "XBTUSDTM"}};
    EXPECT_FALSE(protocol.uta_subscribe_frame(classic).has_value())
        << "the two feeds do not share a frame shape";
    EXPECT_FALSE(protocol.uta_subscribe_frame({}).has_value());
    const std::array<Subscription, 1> nameless{
        Subscription{.topic = k_uta_book_channel, .symbol = ""}};
    EXPECT_FALSE(protocol.uta_subscribe_frame(nameless).has_value());
}

TEST(KucoinSpotTickers, AllTickersStatesBothColumnsTruncatedAtNineDecimals) {
    const auto table = loaded_spot_table();
    static SpotTickerBuffer storage;
    const auto body = fixture("rest_spot_all_tickers.json");
    const auto rows = parse_spot_tickers(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 2U);
    const auto stats = map_ticker_stats((*rows)[0], table, UnixNanos{1});
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base, *parse_decimal_e9("3091.674104240"));
    EXPECT_EQ(stats->volume_24h_quote, *parse_turnover_e6("260392729.755434675"));
    EXPECT_EQ(stats->ts_event, UnixNanos{1'790'330'198'482ULL * 1'000'000ULL});

    WireSpotTicker silent = (*rows)[0];
    silent.vol_value = {};
    const auto base_only = map_ticker_stats(silent, table, UnixNanos{1});
    ASSERT_TRUE(base_only.has_value());
    EXPECT_TRUE(base_only->volume_24h_quote.is_undef());
    silent.vol = {};
    EXPECT_FALSE(map_ticker_stats(silent, table, UnixNanos{1}).has_value());
}

TEST(KucoinUtaBook, ReadsTheDataObjectInAnyOrder) {
    auto storage = std::make_unique<LevelBuffer>();
    const auto wire = parse_uta_book(
        R"({"t":"delta","P":1,"d":{"M":5,"b":[["1","2"]],"x":{"y":[1]},"C":9,"a":[],"O":8,)"
        R"("s":"ETHUSDTM"}})",
        *storage);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "ETHUSDTM");
    EXPECT_EQ(wire->seq_start, 8);
    EXPECT_EQ(wire->seq_end, 9);
    EXPECT_EQ(wire->ts_ns, 5U);
    EXPECT_TRUE(wire->asks.empty());
    ASSERT_EQ(wire->bids.size(), 1U);
    EXPECT_EQ(wire->bids[0].qty, "2");
    EXPECT_FALSE(
        parse_uta_book(R"({"t":"delta","d":{"a":[],"b":[],"C":2,"s":"X"}})", *storage).has_value())
        << "no O";

    const std::string ticker =
        R"({"data":{"ts":5,"bestAskSize":2,"bestBidPrice":"1.5","sequence":7,"bestAskPrice":"2",)"
        R"("bestBidSize":3,"symbol":"ETHUSDTM"},"subject":"tickerV2",)"
        R"("topic":"/contractMarket/tickerV2:ETHUSDTM","type":"message"})";
    const auto event = parse_event(ticker);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::Bbo);
    EXPECT_EQ(event->symbol, "ETHUSDTM");
    const auto bbo = parse_bbo(ticker, MarketType::Perp);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid_price, "1.5");
    EXPECT_EQ(bbo->bid_qty, "3");
    EXPECT_EQ(bbo->ask_qty, "2");
    EXPECT_EQ(bbo->ts_ns, 5U);
    EXPECT_EQ(bbo->sequence, 7);

    const auto change = parse_book_change(
        R"({"data":{"change":"1.5,buy,3","sequence":9},"topic":"/contractMarket/level2:ETHUSDTM"})");
    ASSERT_TRUE(change.has_value());
    EXPECT_EQ(change->sequence, 9);
    EXPECT_EQ(change->ts_ms, 0U) << "no timestamp reads as zero, as before";
}


InstrumentBase classified_contract(std::string_view base_currency, std::string_view asset_class) {
    const std::string symbol = std::string{base_currency} + "USDTM";
    WireContract wire{};
    wire.symbol = symbol;
    wire.base_currency = base_currency;
    wire.quote_currency = "USDT";
    wire.status = "Open";
    wire.type = "FFWCSX";
    wire.tick_size = "0.01";
    wire.lot_size = "1";
    wire.multiplier = "0.01";
    wire.asset_class = asset_class;
    const auto mapped = map_instrument(wire, MarketType::Perp);
    EXPECT_TRUE(mapped.has_value());
    auto base = mapped.has_value() ? base_of(*mapped) : InstrumentBase{};
    classify(VenueId::Kucoin, base);
    return base;
}

TEST(KucoinMap, AssetClassClassifiesTheUnderlying) {
    const auto tsla = classified_contract("TSLA", "STOCK");
    EXPECT_EQ(tsla.asset_class, AssetClass::Equity);
    EXPECT_EQ(tsla.underlying.view(), "TSLA");
    const auto xag = classified_contract("XAG", "METAL");
    EXPECT_EQ(xag.asset_class, AssetClass::Commodity);
    EXPECT_EQ(xag.underlying.view(), "XAG");
    const auto spx = classified_contract("SPX", "CRYPTO");
    EXPECT_EQ(spx.asset_class, AssetClass::Crypto);
    EXPECT_EQ(spx.underlying.view(), "SPX");
    EXPECT_EQ(classified_contract("BOND", "BOND").asset_class, AssetClass::Unknown);
}

} // namespace
} // namespace raikiri::venues::kucoin
