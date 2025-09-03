#include <raikiri/net/signer.hpp>
#include <raikiri/venues/coinbase/book.hpp>
#include <raikiri/venues/coinbase/coinbase_venue.hpp>
#include <raikiri/venues/coinbase/errors.hpp>
#include <raikiri/venues/coinbase/map.hpp>
#include <raikiri/venues/coinbase/rest.hpp>
#include <raikiri/venues/coinbase/sign.hpp>
#include <raikiri/venues/coinbase/wire.hpp>
#include <raikiri/venues/coinbase/ws.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace raikiri::venues::coinbase {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
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

// BTC-USD as Coinbase publishes it: 0.01 tick, 1e-8 step, $1 minimum.
exec::InstrumentTable table_with_btc() {
    exec::InstrumentTable table;
    WireProduct wire{};
    wire.id = "BTC-USD";
    wire.base_currency = "BTC";
    wire.quote_currency = "USD";
    wire.quote_increment = "0.01";
    wire.base_increment = "0.00000001";
    wire.min_market_funds = "1";
    wire.status = "online";
    const auto instrument = map_instrument(wire, MarketType::Spot);
    EXPECT_TRUE(instrument.has_value());
    EXPECT_TRUE(table.add(*instrument).has_value());
    return table;
}

// A 64-byte key behind a printable base64 secret, which is the shape Coinbase
// issues. The decode is the step that goes wrong quietly, so the test data is
// chosen to make a signature over the WRONG key visibly different.
constexpr std::string_view k_key = "raikiri-coinbase-test-key";
constexpr std::string_view k_secret_b64 =
    "cmFpa2lyaS1jb2luYmFzZS10ZXN0LXNlY3JldC1rZXktbWF0ZXJpYWwtMDEyMzQ1Njc4OWFiY2RlZjAxMjM0NQ==";
constexpr std::string_view k_secret_raw =
    "raikiri-coinbase-test-secret-key-material-0123456789abcdef012345";
constexpr std::string_view k_passphrase = "raikiri-passphrase";

// The ULID the OMS mints, and the UUID it must become. Computed independently
// with a Crockford decoder; see rest.hpp for why this is a conversion.
constexpr std::string_view k_ulid = "01K3QW9F2H6M5N8P0R2T4V6X8Z";
constexpr std::string_view k_client_oid = "0198efc4-bc51-450b-9458-181689b3751f";

void load_credentials(net::Signer& signer) {
    signer.set_api_key(k_key);
    // The adapter decodes at credential load; a test that skipped it would
    // pass while production signed with the printable form.
    std::array<char, k_max_secret_bytes> raw{};
    const auto len = decode_secret(k_secret_b64, raw);
    ASSERT_TRUE(len.has_value());
    ASSERT_EQ(std::string_view(raw.data(), *len), k_secret_raw);
    ASSERT_TRUE(signer.secret().assign(std::string_view{raw.data(), *len}).has_value());
}

std::string_view header_of(const net::RestRequest& request, std::string_view name) {
    for (const auto& header : request.headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return {};
}

OrderCmd order_for(const exec::InstrumentTable& table, std::string_view product) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.client_order_id = ClientOrderId::make(1, 42);
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from(k_ulid);
    if (const auto* instrument = table.find(product); instrument != nullptr) {
        cmd.instrument = base_of(*instrument).id;
    }
    return cmd;
}

constexpr UnixNanos k_ingest{1'700'000'000'000'000'000ULL};

// ----------------------------------------------------------- adapter ----

TEST(CoinbaseVenue, RegistersAndBuilds) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto adapter =
        exec::make_venue({.venue = VenueId::Coinbase, .market = MarketType::Spot}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Coinbase);
    EXPECT_NE(adapter->data, nullptr);
}

TEST(CoinbaseVenue, ServesSpotAndRefusesTheMarketsThisEntityDoesNotList) {
    // Decided BEFORE the instrument table is consulted, and each market named
    // separately: "unsupported market" tells an operator nothing about whether
    // to change the config or the adapter.
    EXPECT_FALSE(market_refusal(MarketType::Spot).has_value());
    EXPECT_EQ(market_refusal(MarketType::Perp), BuildError::PerpNotListed);
    EXPECT_EQ(market_refusal(MarketType::Future), BuildError::DatedFuturesNotListed);
    EXPECT_EQ(market_refusal(MarketType::Option), BuildError::OptionsNotListed);
}

TEST(CoinbaseVenue, OrderEntryWithoutCredentialsFailsAtTheConfigNotTheVenue) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter =
        exec::make_venue({.venue = VenueId::Coinbase, .market = MarketType::Spot}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    // Three credentials, not two. AuthFailure sends an operator to the config;
    // Terminal would send them to a venue status page.
    const auto started = adapter->execution->start();
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().cls, exec::ErrorClass::AuthFailure);

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.instrument = InstrumentId::make(VenueId::Coinbase, MarketType::Spot, SymbolId{3});
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from(k_ulid);
    cmd.client_order_id = ClientOrderId::make(1, 42);
    adapter->execution->submit(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);
    EXPECT_EQ(sink.events[0].leaves_qty.raw, 0);

    // No modify anywhere on this API, so an amend is answered rather than
    // dropped: a strategy waiting on a verdict no code will produce waits
    // forever.
    sink.events.clear();
    adapter->execution->amend(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::ModifyRejected);
}

TEST(CoinbaseVenue, PullsThisApiCannotPerformAreRefusedByName) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter =
        exec::make_venue({.venue = VenueId::Coinbase, .market = MarketType::Spot}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    // A coin balance is not a position, and there is no InstrumentId to report
    // one against. An empty vector would read as "flat".
    EXPECT_FALSE(adapter->execution->position_reports(MarketType::Spot).has_value());
    // GET /fills demands order_id or product_id, and this call names a market.
    EXPECT_FALSE(adapter->execution->fill_reports(MarketType::Spot, UnixNanos{0}).has_value());
    EXPECT_FALSE(exec::caps_of(VenueId::Coinbase).position_pull);
    EXPECT_FALSE(exec::caps_of(VenueId::Coinbase).fill_pull);

    // ...and the markets this entity does not list are refused on every pull,
    // not just on order entry.
    EXPECT_FALSE(adapter->execution->open_order_reports(MarketType::Perp).has_value());
    EXPECT_FALSE(adapter->execution->position_reports(MarketType::Future).has_value());
}

TEST(CoinbaseVenue, PerpCatalogueIsRefusedNotEmptied) {
    // Coinbase Exchange lists no derivatives. An empty vector would read to a
    // caller as a venue having a quiet day; only an error says "wrong market".
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter =
        exec::make_venue({.venue = VenueId::Coinbase, .market = MarketType::Spot}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    const auto perps = adapter->instruments->fetch_all(MarketType::Perp);
    ASSERT_FALSE(perps.has_value());
    EXPECT_EQ(perps.error().cls, exec::ErrorClass::Terminal);
}

TEST(CoinbaseVenue, MarketsTheCapsRowDeniesAreRefusedAtBuild) {
    // The caps row says perp = false, so a gateway that asks for perps must
    // be told at startup rather than discover it as an empty catalogue.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    for (const auto market : {MarketType::Perp, MarketType::Future, MarketType::Option}) {
        const auto adapter =
            exec::make_venue({.venue = VenueId::Coinbase, .market = market}, sink, clock);
        ASSERT_FALSE(adapter.has_value()) << to_string(market);
        EXPECT_EQ(adapter.error(), exec::MakeError::UnsupportedMarket) << to_string(market);
    }
    EXPECT_TRUE(
        exec::make_venue({.venue = VenueId::Coinbase, .market = MarketType::Spot}, sink, clock)
            .has_value());
}

// -------------------------------------------------------------- wire ----

TEST(CoinbaseWire, EventDispatchResolvesKindAndProduct) {
    for (const auto& [file, kind] : {std::pair{"ws_ticker.json", EventKind::Ticker},
                                     std::pair{"ws_match.json", EventKind::Match},
                                     std::pair{"ws_last_match.json", EventKind::Match},
                                     std::pair{"ws_subscriptions.json", EventKind::Subscriptions},
                                     std::pair{"ws_error.json", EventKind::Error},
                                     std::pair{"ws_l2_snapshot.json", EventKind::Snapshot},
                                     std::pair{"ws_l2update.json", EventKind::L2Update}}) {
        const auto frame = fixture(file);
        const auto event = parse_event(frame);
        ASSERT_TRUE(event.has_value()) << file;
        EXPECT_EQ(event->kind, kind) << file;
    }
    const auto ticker = fixture("ws_ticker.json");
    const auto event = parse_event(ticker);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->product_id, "BTC-USD");
}

TEST(CoinbaseWire, NonJsonIsAnErrorNotAnEvent) {
    EXPECT_FALSE(parse_event("").has_value());
    EXPECT_FALSE(parse_event("not json at all").has_value());
    EXPECT_FALSE(parse_event("[1,2,3]").has_value());
}

TEST(CoinbaseWire, ParsedViewsPointIntoTheCallersBuffer) {
    // The zero-copy contract: every view must land inside the frame the
    // caller owns, never in simdjson's scratch, which the next frame
    // overwrites. Reading through an unescape buffer is the lifetime bug this
    // pins down.
    const auto frame = fixture("ws_match.json");
    const auto wire = parse_match(frame);
    ASSERT_TRUE(wire.has_value());
    for (const auto view : std::array{wire->product_id, wire->price, wire->size, wire->time}) {
        ASSERT_FALSE(view.empty());
        EXPECT_GE(view.data(), frame.data());
        EXPECT_LE(view.data() + view.size(), frame.data() + frame.size());
    }
}

TEST(CoinbaseWire, TickerCarriesTopOfBook) {
    const auto frame = fixture("ws_ticker.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->product_id, "BTC-USD");
    EXPECT_EQ(wire->best_bid, "64095.97");
    EXPECT_EQ(wire->best_bid_size, "0.20152718");
    EXPECT_EQ(wire->best_ask, "64095.98");
    EXPECT_EQ(wire->best_ask_size, "0.00977492");
    EXPECT_EQ(wire->time, "2026-08-10T20:52:30.757921Z");
    EXPECT_EQ(wire->trade_id, 1'068'956'434);
    EXPECT_EQ(wire->sequence, 134'016'484'716);
}

TEST(CoinbaseWire, MatchSideIsTheMakers) {
    // The fixture pair carries its own proof: this print's price equals the
    // best_ask of the ticker frame with the SAME trade_id, and only a taker
    // who bought prints at the ask. So side "sell" is the resting order's.
    const auto match_frame = fixture("ws_match.json");
    const auto ticker_frame = fixture("ws_ticker.json");
    const auto match = parse_match(match_frame);
    const auto ticker = parse_ticker(ticker_frame);
    ASSERT_TRUE(match.has_value());
    ASSERT_TRUE(ticker.has_value());
    ASSERT_EQ(match->trade_id, ticker->trade_id);
    EXPECT_EQ(match->price, ticker->best_ask);
    EXPECT_TRUE(match->maker_sold);

    const auto other_frame = fixture("ws_match_maker_buy.json");
    const auto other = parse_match(other_frame);
    ASSERT_TRUE(other.has_value());
    EXPECT_FALSE(other->maker_sold);
}

TEST(CoinbaseWire, LastMatchParsesAsAMatch) {
    // The subscribe replay is a real print; treating it as a control frame
    // loses the first trade of every session.
    const auto frame = fixture("ws_last_match.json");
    const auto wire = parse_match(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->product_id, "BTC-USD");
    EXPECT_FALSE(wire->price.empty());
}

TEST(CoinbaseWire, StreamErrorCarriesTheReasonNotJustTheMessage) {
    const auto frame = fixture("ws_error.json");
    const auto err = parse_stream_error(frame);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->message, "Failed to subscribe");
    EXPECT_EQ(err->reason, "NOPE-XYZ is not a valid product");
    EXPECT_EQ(classify_stream(err->message), exec::ErrorClass::Terminal);
}

TEST(CoinbaseWire, SubscriptionAckNamesChannelsTheVenuesWay) {
    AckBuffer storage;
    const auto plain = fixture("ws_subscriptions.json");
    const auto confirmed = parse_subscriptions(plain, storage);
    ASSERT_TRUE(confirmed.has_value());
    ASSERT_EQ(confirmed->size(), 2U);
    EXPECT_EQ((*confirmed)[0], "ticker");
    EXPECT_EQ((*confirmed)[1], "matches");

    // A level2_batch request is acked as "level2_50": comparing the ack to
    // what was asked for would read a live channel as a refused one.
    const auto renamed = fixture("ws_subscriptions_level2_renamed.json");
    const auto l2 = parse_subscriptions(renamed, storage);
    ASSERT_TRUE(l2.has_value());
    ASSERT_EQ(l2->size(), 3U);
    EXPECT_EQ((*l2)[2], "level2_50");
}

TEST(CoinbaseWire, EmptySubscriptionAckIsTheRefusal) {
    // This frame arrives beside the error frame. An empty channel list is the
    // only positive statement that nothing is live on the connection.
    AckBuffer storage;
    const auto frame = fixture("ws_subscriptions_empty.json");
    const auto confirmed = parse_subscriptions(frame, storage);
    ASSERT_TRUE(confirmed.has_value());
    EXPECT_TRUE(confirmed->empty());
}

TEST(CoinbaseWire, TruncatedFrameFailsRatherThanFillingPartially) {
    // A parser that returned a struct with a plausible zero price here would
    // publish a trade at zero. It must fail instead.
    auto frame = fixture("ws_match.json");
    frame.resize(frame.size() / 2);
    EXPECT_FALSE(parse_match(frame).has_value());
}

TEST(CoinbaseWire, MissingFieldFailsTheFrame) {
    const std::string frame = R"({"type":"match","product_id":"BTC-USD","price":"1.00"})";
    const auto wire = parse_match(frame);
    ASSERT_FALSE(wire.has_value());
    EXPECT_EQ(wire.error(), WireError::MissingField);
}

TEST(CoinbaseWire, ProductsParseIncludingTheRowsTheMapperWillRefuse) {
    static ProductBuffer storage;
    const auto body = fixture("rest_products.json");
    const auto rows = parse_products(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);
    EXPECT_EQ((*rows)[0].id, "BTC-USD");
    EXPECT_EQ((*rows)[0].quote_increment, "0.01");
    EXPECT_EQ((*rows)[0].base_increment, "0.00000001");
    EXPECT_EQ((*rows)[0].min_market_funds, "1");
    EXPECT_EQ((*rows)[0].status, "online");
    EXPECT_FALSE((*rows)[0].trading_disabled);
    EXPECT_TRUE((*rows)[2].limit_only);
    EXPECT_EQ((*rows)[3].status, "delisted");
    EXPECT_TRUE((*rows)[3].trading_disabled);
}

TEST(CoinbaseWire, SingleProductIsABareObject) {
    const auto body = fixture("rest_product_btc.json");
    const auto row = parse_product(body);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->id, "BTC-USD");
    EXPECT_EQ(row->base_currency, "BTC");
    EXPECT_EQ(row->quote_currency, "USD");
}

TEST(CoinbaseWire, ServerTimeReadsTheStringNotTheFloat) {
    const auto body = fixture("rest_time.json");
    const auto iso = parse_server_time(body);
    ASSERT_TRUE(iso.has_value());
    EXPECT_EQ(*iso, "2026-08-10T20:51:41.541Z");
}

TEST(CoinbaseWire, RestErrorIsJustAMessage) {
    const auto body = fixture("rest_error_not_found.json");
    const auto message = parse_rest_error(body);
    ASSERT_TRUE(message.has_value());
    EXPECT_EQ(*message, "NotFound");
    // The body carries no code, so the class comes entirely from the status.
    EXPECT_EQ(classify_http(404), exec::ErrorClass::Terminal);
}

// --------------------------------------------------------------- iso ----

TEST(CoinbaseIso, ParsesEveryFractionWidthThisApiSends) {
    // Three widths from three endpoints, all live: /time gives 3, the ticker
    // channel 6, /products/*/book 9.
    const auto milli = parse_iso8601("2026-08-10T20:51:41.541Z");
    ASSERT_TRUE(milli.has_value());
    EXPECT_EQ(milli->ns, 1'786'395'101'541'000'000ULL);

    const auto micro = parse_iso8601("2026-08-10T20:52:30.757921Z");
    ASSERT_TRUE(micro.has_value());
    EXPECT_EQ(micro->ns, 1'786'395'150'757'921'000ULL);

    const auto nano = parse_iso8601("2026-08-10T20:51:41.173423307Z");
    ASSERT_TRUE(nano.has_value());
    EXPECT_EQ(nano->ns, 1'786'395'101'173'423'307ULL);

    // No fraction at all is still a valid instant.
    const auto whole = parse_iso8601("2026-08-10T20:51:41Z");
    ASSERT_TRUE(whole.has_value());
    EXPECT_EQ(whole->ns, 1'786'395'101'000'000'000ULL);
}

TEST(CoinbaseIso, HandlesLeapYearsAndEpochBoundaries) {
    const auto epoch = parse_iso8601("1970-01-01T00:00:00Z");
    ASSERT_TRUE(epoch.has_value());
    EXPECT_EQ(epoch->ns, 0U);

    // 2000 is a leap year and 1900 is not; a naive %4 rule gets this day
    // wrong by one and every timestamp after it with it.
    const auto leap = parse_iso8601("2000-02-29T12:00:00Z");
    ASSERT_TRUE(leap.has_value());
    EXPECT_EQ(leap->ns, 951'825'600'000'000'000ULL);

    const auto y2024 = parse_iso8601("2024-12-31T23:59:59.999999999Z");
    ASSERT_TRUE(y2024.has_value());
    EXPECT_EQ(y2024->ns, 1'735'689'599'999'999'999ULL);
}

TEST(CoinbaseIso, RefusesAnythingNotThisShape) {
    for (const auto* text : {
             "",
             "2026-08-10",
             "2026-08-10T20:51:41",     // fine actually; see below
             "2026-08-10 20:51:41Z",    // space separator
             "2026-08-10T20:51:41.Z",   // dot with no digits
             "2026-13-10T20:51:41Z",    // month 13
             "2026-08-10T24:51:41Z",    // hour 24
             "2026-08-10T20:51:60Z",    // leap second: no epoch form
             "2026-08-10T20:51:41+0100" // an offset would parse to the
                                        // wrong instant if assumed UTC
         }) {
        const std::string_view view{text};
        const auto parsed = parse_iso8601(view);
        if (view == "2026-08-10T20:51:41") {
            EXPECT_TRUE(parsed.has_value()) << text; // bare, unzoned, still UTC
            continue;
        }
        EXPECT_FALSE(parsed.has_value()) << text;
    }
    // 1786395101 as digits is not a timestamp on this venue, and must not be
    // mistaken for one.
    EXPECT_FALSE(parse_iso8601("1786395101541").has_value());
}

TEST(CoinbaseIso, UnreadableTimeFallsBackToIngestNotZero) {
    // A zero ts_event propagating into latency histograms poisons them.
    EXPECT_EQ(iso_to_unix("garbage", k_ingest).ns, k_ingest.ns);
    EXPECT_EQ(iso_to_unix("", k_ingest).ns, k_ingest.ns);
}

// --------------------------------------------------------------- map ----

TEST(CoinbaseMap, TickerBecomesTopOfBook) {
    const auto table = table_with_btc();
    const auto frame = fixture("ws_ticker.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());

    const auto bbo = map_bbo(*wire, table, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 6'409'597); // 64095.97 / 0.01
    EXPECT_EQ(bbo->ask.raw, 6'409'598);
    EXPECT_EQ(bbo->bid_qty.raw, 20'152'718); // 0.20152718 / 1e-8
    EXPECT_EQ(bbo->ask_qty.raw, 977'492);
    EXPECT_EQ(bbo->ts_event.ns, 1'786'395'150'757'921'000ULL);
    EXPECT_EQ(bbo->ts_init.ns, k_ingest.ns);
    EXPECT_EQ(bbo->instrument.market(), MarketType::Spot);
}

TEST(CoinbaseMap, MatchBecomesATradeWithTheAggressorFlipped) {
    const auto table = table_with_btc();
    const auto sell_frame = fixture("ws_match.json");
    const auto maker_sell = parse_match(sell_frame);
    ASSERT_TRUE(maker_sell.has_value());
    const auto lifted = map_trade(*maker_sell, table, k_ingest);
    ASSERT_TRUE(lifted.has_value());
    // Maker sold at the ask, so the AGGRESSOR bought. Passing side through
    // unflipped is how a signed-flow series comes out exactly backwards.
    EXPECT_EQ(lifted->aggressor, Side::Buy);
    EXPECT_EQ(lifted->price.raw, 6'409'598);
    EXPECT_EQ(lifted->qty.raw, 1'660); // 0.0000166 / 1e-8

    const auto buy_frame = fixture("ws_match_maker_buy.json");
    const auto maker_buy = parse_match(buy_frame);
    ASSERT_TRUE(maker_buy.has_value());
    const auto hit = map_trade(*maker_buy, table, k_ingest);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->aggressor, Side::Sell);
}

TEST(CoinbaseMap, UnknownProductIsDropped) {
    const auto table = table_with_btc();
    WireMatch wire{};
    wire.product_id = "ETH-USD"; // never added to this table
    wire.price = "1878.19";
    wire.size = "1.00000000";
    wire.time = "2026-08-10T20:51:41.854071Z";
    EXPECT_FALSE(map_trade(wire, table, k_ingest).has_value());
}

TEST(CoinbaseMap, PriceOffTheTickIsDroppedNotRounded) {
    // A price that does not divide by the tick means Coinbase changed a
    // product's increments under us. Rounding would move the print.
    const auto table = table_with_btc();
    WireTicker wire{};
    wire.product_id = "BTC-USD";
    wire.best_bid = "64095.975"; // half a 0.01 tick past
    wire.best_bid_size = "1.00000000";
    wire.best_ask = "64095.98";
    wire.best_ask_size = "1.00000000";
    wire.time = "2026-08-10T20:52:30.757921Z";
    EXPECT_FALSE(map_bbo(wire, table, k_ingest).has_value());

    wire.best_bid = "64095.97";
    wire.best_bid_size = "0.000000005"; // half a 1e-8 step
    EXPECT_FALSE(map_bbo(wire, table, k_ingest).has_value());
}

TEST(CoinbaseMap, OnlineProductBecomesASpotInstrument) {
    static ProductBuffer storage;
    const auto body = fixture("rest_products.json");
    const auto rows = parse_products(body, storage);
    ASSERT_TRUE(rows.has_value());

    const auto btc = map_instrument((*rows)[0], MarketType::Spot);
    ASSERT_TRUE(btc.has_value());
    // The whole point of this venue: the SPOT arm of the variant, not perp.
    EXPECT_TRUE(std::holds_alternative<SpotInstrument>(*btc));
    const auto& base = base_of(*btc);
    EXPECT_EQ(base.exchange_symbol.view(), "BTC-USD"); // hyphenated, kept verbatim
    EXPECT_EQ(base.tick_size.raw, 10'000'000);         // 0.01 e9
    EXPECT_EQ(base.lot_size.raw, 10);                  // 1e-8 e9
    EXPECT_EQ(base.contract_value.raw, k_e9);          // spot: one unit is one unit
    EXPECT_EQ(base.min_notional.raw, k_e9);            // min_market_funds is already quote
    EXPECT_EQ(base.id.market(), MarketType::Spot);
}

TEST(CoinbaseMap, RefusesRowsThatAreNotTradeable) {
    static ProductBuffer storage;
    const auto body = fixture("rest_products.json");
    const auto rows = parse_products(body, storage);
    ASSERT_TRUE(rows.has_value());

    // DDX-EUR is delisted and trading_disabled.
    EXPECT_FALSE(map_instrument((*rows)[3], MarketType::Spot).has_value());

    // Either flag alone is enough; the venue is not obliged to move both.
    auto halted = (*rows)[0];
    halted.status = "delisted";
    EXPECT_FALSE(map_instrument(halted, MarketType::Spot).has_value());
    halted = (*rows)[0];
    halted.trading_disabled = true;
    EXPECT_FALSE(map_instrument(halted, MarketType::Spot).has_value());
}

TEST(CoinbaseMap, KeepsLimitOnlyProductsBecauseTheyStillQuote) {
    // limit_only restricts ORDER ENTRY, which this adapter does not do, while
    // the product goes on printing. 21 online products carry the flag today
    // and dropping their market data would cost real coverage.
    static ProductBuffer storage;
    const auto body = fixture("rest_products.json");
    const auto rows = parse_products(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_TRUE((*rows)[2].limit_only);
    EXPECT_TRUE(map_instrument((*rows)[2], MarketType::Spot).has_value());
}

TEST(CoinbaseMap, RefusesEveryMarketButSpot) {
    static ProductBuffer storage;
    const auto body = fixture("rest_products.json");
    const auto rows = parse_products(body, storage);
    ASSERT_TRUE(rows.has_value());
    for (const auto market : {MarketType::Perp, MarketType::Future, MarketType::Option}) {
        EXPECT_FALSE(map_instrument((*rows)[0], market).has_value());
    }
}

TEST(CoinbaseMap, UnusableIncrementsKeepAnInstrumentOutOfTheTable) {
    // Every later price conversion divides by the tick, so a row that cannot
    // state one must never be loaded.
    WireProduct wire{};
    wire.id = "BTC-USD";
    wire.status = "online";
    wire.base_increment = "0.00000001";
    wire.min_market_funds = "1";
    for (const auto* tick : {"", "0", "-0.01", "abc"}) {
        wire.quote_increment = tick;
        EXPECT_FALSE(map_instrument(wire, MarketType::Spot).has_value()) << tick;
    }
    wire.quote_increment = "0.01";
    for (const auto* step : {"", "0", "abc"}) {
        wire.base_increment = step;
        EXPECT_FALSE(map_instrument(wire, MarketType::Spot).has_value()) << step;
    }
}

// -------------------------------------------------------------- book ----

// The parsed views point into the frame text and into the level buffer, so
// both have to outlive the wire struct that came out of them.
struct BookFrame {
    std::string text;
    std::unique_ptr<LevelBuffer> levels{std::make_unique<LevelBuffer>()};
    std::unique_ptr<ChangeBuffer> changes{std::make_unique<ChangeBuffer>()};
};

WireL2Snapshot snapshot_of(BookFrame& frame) {
    frame.text = fixture("ws_l2_snapshot.json");
    const auto wire = parse_l2_snapshot(frame.text, *frame.levels);
    EXPECT_TRUE(wire.has_value());
    return wire.value_or(WireL2Snapshot{});
}

WireL2Update update_of(BookFrame& frame, std::string_view name) {
    frame.text = fixture(name);
    const auto wire = parse_l2_update(frame.text, *frame.changes);
    EXPECT_TRUE(wire.has_value());
    return wire.value_or(WireL2Update{});
}

Book book_for(const exec::InstrumentTable& table) {
    const auto* instrument = table.find("BTC-USD");
    EXPECT_NE(instrument, nullptr);
    const auto& base = base_of(*instrument);
    return Book{base.id, base.tick_size, base.lot_size};
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

TEST(CoinbaseBook, ChangesBeforeTheSnapshotPublishNothing) {
    // level2_batch sends changes, not state. A book built from one would be a
    // handful of levels presented to shm as the whole market.
    const auto table = table_with_btc();
    auto book = book_for(table);
    BookFrame update;
    EXPECT_EQ(book.apply(update_of(update, "ws_l2update.json")), BookResult::Ignored);
    EXPECT_FALSE(book.synced());
    EXPECT_FALSE(book.snapshot(k_ingest, k_ingest).has_value());
}

TEST(CoinbaseBook, SnapshotAndChangesMakeABookSharedMemoryCanTake) {
    const auto table = table_with_btc();
    auto book = book_for(table);
    BookFrame snapshot_frame;
    BookFrame update;

    const auto wire = snapshot_of(snapshot_frame);
    EXPECT_EQ(wire.bids.size(), 150U);
    EXPECT_EQ(wire.asks.size(), 150U);
    EXPECT_EQ(book.reset(wire), BookResult::Resynced);

    const auto out = book.snapshot(UnixNanos{7}, UnixNanos{9});
    ASSERT_TRUE(out.has_value());
    expect_coherent(*out);
    EXPECT_EQ(out->bid_count, k_book_levels);
    EXPECT_EQ(out->ask_count, k_book_levels);
    EXPECT_EQ(out->bids[0].price.raw, 6'401'728); // 64017.28 over the 0.01 tick
    EXPECT_EQ(out->asks[0].price.raw, 6'401'729);
    EXPECT_EQ(out->bids[0].qty.raw, 9'994'431); // 0.09994431 over the 1e-8 step
    EXPECT_EQ(out->bids[99].price.raw, 6'398'370);
    EXPECT_EQ(out->asks[99].price.raw, 6'404'224);
    // Nothing on this channel carries an ordering token; Coinbase's sequence
    // numbers live on the `full` channel, which is a different subscription.
    EXPECT_EQ(out->venue_seq, 0ULL);
    EXPECT_EQ(out->ts_event.ns, 7ULL);

    const auto changes = update_of(update, "ws_l2update_chained.json");
    EXPECT_EQ(changes.changes.size(), 16U);
    EXPECT_EQ(book.apply(changes), BookResult::Applied);
    const auto after = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(after.has_value());
    expect_coherent(*after);
}

TEST(CoinbaseBook, AChangeOutsideTheSnapshotsBandIsNotCarried) {
    // The fixture keeps 150 levels a side of a book that was 45,805 deep, so
    // its floor is 63962.57 — and one real change in the frame that followed
    // it names a bid at 56015.12. Nothing in the snapshot vouched for that
    // price, so inserting it would make the book look deeper than it is known
    // to be. binance's seeded book bands itself for the same reason.
    const auto table = table_with_btc();
    auto book = book_for(table);
    BookFrame snapshot_frame;
    BookFrame update;
    EXPECT_EQ(book.reset(snapshot_of(snapshot_frame)), BookResult::Resynced);
    const auto held = book.bids().size();

    EXPECT_EQ(book.apply(update_of(update, "ws_l2update_chained.json")), BookResult::Applied);
    EXPECT_EQ(book.outside_band(), 1U);
    EXPECT_EQ(book.bids().size(), held);
    EXPECT_EQ(book.bids().back().price.raw, 6'396'257) << "63962.57, the snapshot's own floor";
}

TEST(CoinbaseBook, ASizeOfZeroDeletesTheLevel) {
    const auto table = table_with_btc();
    auto book = book_for(table);
    BookFrame snapshot_frame;
    EXPECT_EQ(book.reset(snapshot_of(snapshot_frame)), BookResult::Resynced);

    const auto before = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(before.has_value());
    const auto next_down = before->bids[1].price.raw;

    // The touch, lifted. A book that read the zero as a size would leave the
    // best bid resting at nothing.
    const std::array<WireChange, 1> gone{
        WireChange{.price = "64017.28", .qty = "0.00000000", .bid = true}};
    WireL2Update lift{};
    lift.product_id = "BTC-USD";
    lift.changes = gone;
    EXPECT_EQ(book.apply(lift), BookResult::Applied);

    const auto after = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(after.has_value());
    expect_coherent(*after);
    EXPECT_EQ(after->bids[0].price.raw, next_down);
    EXPECT_EQ(book.bids().size(), 149U);
}

TEST(CoinbaseBook, ALevelOffTheIncrementInvalidatesTheWholeBook) {
    // The rule that separates a delta venue from a snapshot venue. Dropping
    // one level here would leave a book that disagrees with Coinbase's until
    // the next reconnect, and this channel has no sequence that would say so.
    const auto table = table_with_btc();
    auto book = book_for(table);
    BookFrame snapshot_frame;
    EXPECT_EQ(book.reset(snapshot_of(snapshot_frame)), BookResult::Resynced);

    const std::array<WireChange, 1> off_tick{
        WireChange{.price = "64017.285", .qty = "1.0", .bid = true}};
    WireL2Update bad{};
    bad.product_id = "BTC-USD";
    bad.changes = off_tick;
    EXPECT_EQ(book.apply(bad), BookResult::BadLevel);
    EXPECT_FALSE(book.synced());
    EXPECT_FALSE(book.snapshot(k_ingest, k_ingest).has_value());

    // A size finer than the 1e-8 base increment is refused the same way.
    auto again = book_for(table);
    BookFrame again_snapshot;
    EXPECT_EQ(again.reset(snapshot_of(again_snapshot)), BookResult::Resynced);
    const std::array<WireChange, 1> sub_lot{
        WireChange{.price = "64017.28", .qty = "0.000000005", .bid = true}};
    WireL2Update fine{};
    fine.product_id = "BTC-USD";
    fine.changes = sub_lot;
    EXPECT_EQ(again.apply(fine), BookResult::BadLevel);
    EXPECT_FALSE(again.snapshot(k_ingest, k_ingest).has_value());
}

// ---------------------------------------------------------------- ws ----

TEST(CoinbaseWs, ChannelsCoverExactlyWhatThisVenueStreams) {
    EXPECT_EQ(channel_for(exec::SubKind::Bbo), "ticker");
    EXPECT_EQ(channel_for(exec::SubKind::Trades), "matches");
    // Funding is empty because spot HAS no funding — the DataClient default
    // then declines, which is the honest answer rather than a faked zero.
    EXPECT_TRUE(channel_for(exec::SubKind::Funding).empty());
    EXPECT_TRUE(channel_for(exec::SubKind::MarkPrice).empty());
    // level2 proper needs credentials; level2_batch does not, and the ack
    // renames it "level2_50" while delivering the whole book anyway.
    EXPECT_EQ(channel_for(exec::SubKind::Depth), "level2_batch");
    EXPECT_TRUE(channel_for(exec::SubKind::Kline).empty());
}

TEST(CoinbaseWs, SubscribeFrameGroupsProductsUnderOneChannel) {
    Protocol protocol;
    const std::array<Subscription, 3> subs{
        Subscription{.channel = "ticker", .product_id = "BTC-USD"},
        Subscription{.channel = "matches", .product_id = "BTC-USD"},
        Subscription{.channel = "ticker", .product_id = "ETH-USD"}};
    const auto frame = protocol.subscribe_frame(subs);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->consumed, 3U);
    EXPECT_EQ(frame->text,
              R"({"type":"subscribe","channels":[{"name":"ticker","product_ids":["BTC-USD",)"
              R"("ETH-USD"]},{"name":"matches","product_ids":["BTC-USD"]}]})");
}

TEST(CoinbaseWs, UnsubscribeDiffersOnlyByType) {
    Protocol protocol;
    const std::array<Subscription, 1> subs{
        Subscription{.channel = "ticker", .product_id = "BTC-USD"}};
    const auto frame = protocol.unsubscribe_frame(subs);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->text,
              R"({"type":"unsubscribe","channels":[{"name":"ticker","product_ids":["BTC-USD"]}]})");
}

TEST(CoinbaseWs, DeclinedKindsNeverReachTheWire) {
    // channel_for() returns empty for those, and subscribing to the literal
    // "" earns a "Failed to subscribe" that looks like a venue fault.
    Protocol protocol;
    const std::array<Subscription, 1> empty_channel{
        Subscription{.channel = {}, .product_id = "BTC-USD"}};
    EXPECT_FALSE(protocol.subscribe_frame(empty_channel).has_value());
    EXPECT_FALSE(protocol.subscribe_frame({}).has_value());
}

TEST(CoinbaseWs, OversizeBatchIsRefusedRatherThanTruncated) {
    // A truncated frame would be valid JSON for FEWER products than asked
    // for, and the missing ones would simply never tick.
    Protocol protocol;
    std::vector<Subscription> many;
    const std::string long_id(200, 'A');
    many.reserve(k_max_subscriptions_per_frame);
    for (std::size_t i = 0; i < k_max_subscriptions_per_frame; ++i) {
        many.push_back({.channel = "ticker", .product_id = long_id});
    }
    const auto frame = protocol.subscribe_frame(many);
    ASSERT_FALSE(frame.has_value());
    EXPECT_EQ(frame.error(), WsBuildError::BufferTooSmall);
}

// -------------------------------------------------------------- rest ----

TEST(CoinbaseRest, TargetsMatchTheEndpointsCaptured) {
    RequestBuilder builder;
    EXPECT_EQ(builder.products().target, "/products");
    EXPECT_EQ(builder.server_time().target, "/time");
    EXPECT_EQ(builder.product("BTC-USD").target, "/products/BTC-USD");
    EXPECT_EQ(builder.book("BTC-USD", 1).target, "/products/BTC-USD/book?level=1");
    EXPECT_EQ(rest_host(), "api.exchange.coinbase.com");
    EXPECT_EQ(stream_host(), "ws-feed.exchange.coinbase.com");
}

TEST(CoinbaseErrors, StatusIsTheOnlyMachineReadablePartOfAFailure) {
    EXPECT_EQ(classify_http(0), exec::ErrorClass::Connectivity);
    EXPECT_EQ(classify_http(429), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify_http(401), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify_http(500), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify_http(503), exec::ErrorClass::Retryable);
    // Unknown conditions default to Terminal: an unknown refusal repeated at
    // full speed is how an IP earns a ban.
    EXPECT_EQ(classify_http(400), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify_http(418), exec::ErrorClass::Terminal);
}

// ------------------------------------------------------ order entry ----

TEST(CoinbaseClientOid, TheUlidBecomesAUuidBecauseAnythingElseIsIgnored) {
    // Coinbase's own words: a client_oid that is not a variant 1 UUIDv4 "will
    // be accepted via REST but NOT processed and will not be visible in future
    // REST calls". A silently ignored id is worse than a refused one, so the
    // 26-character ULID is CONVERTED — both are 128 bits — rather than sent.
    const auto id = *ExchangeClientOrderId::from(k_ulid);
    const auto uuid = client_oid_of(id);
    ASSERT_TRUE(uuid.has_value());
    EXPECT_EQ(std::string_view(uuid->data(), uuid->size()), k_client_oid);
    EXPECT_EQ(uuid->size(), k_uuid_len);

    // Version 4 and variant 1, which is what the venue checks for.
    EXPECT_EQ((*uuid)[14], '4');
    EXPECT_TRUE((*uuid)[19] == '8' || (*uuid)[19] == '9' || (*uuid)[19] == 'a' ||
                (*uuid)[19] == 'b');
    // Lowercase hyphenated, grouped 8-4-4-4-12.
    EXPECT_EQ((*uuid)[8], '-');
    EXPECT_EQ((*uuid)[13], '-');
    EXPECT_EQ((*uuid)[18], '-');
    EXPECT_EQ((*uuid)[23], '-');

    // Deterministic, so GET /orders/client:<uuid> resolves with no local table.
    EXPECT_EQ(client_oid_of(id), uuid);

    // Anything that is not a ULID is refused rather than sent as-is.
    EXPECT_FALSE(client_oid_of(ExchangeClientOrderId{}).has_value());
    EXPECT_FALSE(client_oid_of(*ExchangeClientOrderId::from("too-short")).has_value());
    // 'I', 'L', 'O' and 'U' are not in Crockford's alphabet.
    EXPECT_FALSE(
        client_oid_of(*ExchangeClientOrderId::from("01K3QW9F2H6M5N8P0R2T4V6XIL")).has_value());
    // A first character above 7 would carry more than 128 bits.
    EXPECT_FALSE(
        client_oid_of(*ExchangeClientOrderId::from("Z1K3QW9F2H6M5N8P0R2T4V6X8Z")).has_value());
}

TEST(CoinbaseSign, TheSecretIsBase64AndTheHmacKeyIsItsBytes) {
    std::array<char, k_max_secret_bytes> raw{};
    const auto len = decode_secret(k_secret_b64, raw);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(*len, k_secret_raw.size());
    EXPECT_EQ(std::string_view(raw.data(), *len), k_secret_raw);

    // Not base64 at all, which is what a pasted-wrong key looks like.
    EXPECT_FALSE(decode_secret("not base64!!", raw).has_value());
    EXPECT_FALSE(decode_secret("abc", raw).has_value()); // length not a multiple of 4
    EXPECT_FALSE(decode_secret("", raw).has_value());

    // timestamp + METHOD + requestPath + body, concatenated with NOTHING
    // between them.
    std::array<char, 256> buffer{};
    EXPECT_EQ(prehash("1787751010", net::HttpMethod::Get, "/orders?status=open", {}, buffer),
              "1787751010GET/orders?status=open");
    // SECONDS, not the milliseconds every other venue here wants.
    std::array<char, 24> stamp{};
    EXPECT_EQ(timestamp_s(UnixNanos{1'787'751'010'667'000'000ULL}, stamp), "1787751010");
}

TEST(CoinbaseSignedRest, CreateOrderSignsTheBodyItActuallySends) {
    const auto table = table_with_btc();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer, k_passphrase};

    auto cmd = order_for(table, "BTC-USD");
    cmd.side = Side::Buy;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.flags = OrderFlags::PostOnly;
    cmd.qty = QtyLots::from_raw(50'000'000);     // 0.5 BTC in 1e-8 steps
    cmd.price = PriceTicks::from_raw(6'350'000); // 63500.00 in 0.01 ticks

    const auto request = builder.create_order(cmd, table, UnixNanos{1'787'751'010'667'000'000ULL});
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->method, net::HttpMethod::Post);
    EXPECT_EQ(request->target, "/orders");
    EXPECT_EQ(request->body,
              R"({"client_oid":"0198efc4-bc51-450b-9458-181689b3751f","product_id":"BTC-USD",)"
              R"("side":"buy","type":"limit","size":"0.5","price":"63500",)"
              R"("time_in_force":"GTC","post_only":true})");
    EXPECT_EQ(builder.last_prehash(),
              std::string{"1787751010POST/orders"} + std::string{request->body});
    // Digest computed independently in Python over the same bytes with the
    // DECODED key, so this pins the decode as well as the HMAC.
    EXPECT_EQ(header_of(*request, "CB-ACCESS-SIGN"),
              "A9Hfp8A4oD2YiaHt7OFP8wO/ebpnLYOT74HEGwy7fDk=");
    EXPECT_EQ(header_of(*request, "CB-ACCESS-KEY"), k_key);
    EXPECT_EQ(header_of(*request, "CB-ACCESS-TIMESTAMP"), "1787751010");
    EXPECT_EQ(header_of(*request, "CB-ACCESS-PASSPHRASE"), k_passphrase);
}

TEST(CoinbaseSignedRest, MarketOrdersAndTimeInForceCarryTheVenuesOwnRules) {
    const auto table = table_with_btc();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer, k_passphrase};
    const UnixNanos now{1'787'751'010'667'000'000ULL};

    // `size` is BASE currency on both sides, so a market BUY needs no
    // quote-denominated guess and — unlike gate and htx — is not refused.
    auto market = order_for(table, "BTC-USD");
    market.order_type = OrderType::Market;
    market.qty = QtyLots::from_raw(25'000'000);
    const auto taker = builder.create_order(market, table, now);
    ASSERT_TRUE(taker.has_value());
    EXPECT_EQ(taker->body,
              R"({"client_oid":"0198efc4-bc51-450b-9458-181689b3751f","product_id":"BTC-USD",)"
              R"("side":"buy","type":"market","size":"0.25"})");

    // post_only beside IOC is REFUSED by the venue, not ignored, so a
    // maker-only order that is not GTC simply is not one.
    auto ioc = order_for(table, "BTC-USD");
    ioc.tif = TimeInForce::Ioc;
    ioc.flags = OrderFlags::PostOnly | OrderFlags::ReduceOnly;
    ioc.qty = QtyLots::from_raw(50'000'000);
    ioc.price = PriceTicks::from_raw(6'350'000);
    const auto immediate = builder.create_order(ioc, table, now);
    ASSERT_TRUE(immediate.has_value());
    EXPECT_NE(std::string{immediate->body}.find(R"("time_in_force":"IOC")"), std::string::npos);
    EXPECT_EQ(std::string{immediate->body}.find("post_only"), std::string::npos);
    // ReduceOnly has no field here: a spot balance cannot be short.
    EXPECT_EQ(std::string{immediate->body}.find("reduce_only"), std::string::npos);
}

TEST(CoinbaseSignedRest, CancelPrefersTheVenueIdAndNamespacesTheClientOne) {
    const auto table = table_with_btc();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer, k_passphrase};
    const UnixNanos now{1'787'751'010'667'000'000ULL};

    auto cmd = order_for(table, "BTC-USD");
    cmd.kind = CmdKind::Cancel;
    const auto by_client = builder.cancel_order(cmd, table, now);
    ASSERT_TRUE(by_client.has_value());
    // The `client:` namespace is what tells Coinbase the id is ours; the bare
    // form is read as a venue id and answers 404.
    EXPECT_EQ(by_client->target,
              "/orders/client:0198efc4-bc51-450b-9458-181689b3751f?product_id=BTC-USD");
    EXPECT_EQ(builder.last_prehash(),
              std::string{"1787751010DELETE"} + std::string{by_client->target});
    EXPECT_EQ(header_of(*by_client, "CB-ACCESS-SIGN"),
              "177FdGidwD6bETBLZQwxC80ykHNmC7iqvzCFEyYZn+I=");

    cmd.target_exchange_order_id = *ExchangeOrderId::from("a9625b04-fc66-4999-a876-543c3684d702");
    const auto by_id = builder.cancel_order(cmd, table, now);
    ASSERT_TRUE(by_id.has_value());
    EXPECT_EQ(by_id->target, "/orders/a9625b04-fc66-4999-a876-543c3684d702?product_id=BTC-USD");

    cmd.target_exchange_order_id = {};
    cmd.exchange_client_order_id = {};
    EXPECT_EQ(builder.cancel_order(cmd, table, now).error(), BuildError::NoOrderTarget);
}

TEST(CoinbaseSignedRest, PullsAndRefusalsAreNamedRatherThanImplied) {
    const auto table = table_with_btc();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer, k_passphrase};
    const UnixNanos now{1'787'751'010'667'000'000ULL};

    // `status` is REQUIRED, and `pending` belongs beside `open`: leaving it
    // out reports a live order as absent for as long as the engine holds it.
    EXPECT_EQ(builder.open_orders(MarketType::Spot, now)->target,
              "/orders?status=open&status=pending&limit=1000");
    EXPECT_EQ(
        builder.query_order(*ExchangeClientOrderId::from(k_ulid), MarketType::Spot, now)->target,
        "/orders/client:0198efc4-bc51-450b-9458-181689b3751f");

    auto cmd = order_for(table, "BTC-USD");
    cmd.qty = QtyLots::from_raw(50'000'000);
    cmd.price = PriceTicks::from_raw(6'350'000);
    // No modify on this REST API at all. An amend that looked like it worked
    // would be a lie about the book.
    EXPECT_EQ(builder.amend_order(cmd, table, now).error(), BuildError::AmendUnsupported);

    cmd.instrument = InstrumentId::make(VenueId::Coinbase, MarketType::Perp, SymbolId{0});
    EXPECT_EQ(builder.create_order(cmd, table, now).error(), BuildError::PerpNotListed);
    EXPECT_EQ(builder.open_orders(MarketType::Perp, now).error(), BuildError::PerpNotListed);

    // An id that is not a ULID cannot become a UUID, and must not travel raw.
    auto raw_id = order_for(table, "BTC-USD");
    raw_id.qty = QtyLots::from_raw(50'000'000);
    raw_id.price = PriceTicks::from_raw(6'350'000);
    raw_id.exchange_client_order_id = *ExchangeClientOrderId::from("not-a-ulid");
    EXPECT_EQ(builder.create_order(raw_id, table, now).error(), BuildError::ClientOrderIdNotAUlid);
}

TEST(CoinbaseSignedRest, RefusesToSignWithoutEveryCredential) {
    net::Signer signer;
    load_credentials(signer);
    // The passphrase is a third credential and the one most often left unset;
    // without it every request is 401, which reads like a wrong secret.
    SignedRequestBuilder no_phrase{signer, {}};
    EXPECT_EQ(no_phrase.open_orders(MarketType::Spot, UnixNanos{1}).error(), BuildError::NotSigned);

    const net::Signer bare;
    SignedRequestBuilder no_key{bare, k_passphrase};
    EXPECT_EQ(no_key.open_orders(MarketType::Spot, UnixNanos{1}).error(), BuildError::NotSigned);
}

// ------------------------------------------------- order event mapping ----

TEST(CoinbaseMap, AnAcknowledgedOrderIsAcceptedWhetherPendingOrResting) {
    const auto table = table_with_btc();
    const auto id = *ExchangeClientOrderId::from(k_ulid);

    const auto body = fixture("rest_order_ack.json");
    const auto order = parse_order(body);
    ASSERT_TRUE(order.has_value());
    EXPECT_EQ(order->status, "pending");
    EXPECT_EQ(order->client_oid, k_client_oid);

    const auto event = map_order_event(*order, table, id, k_ingest);
    ASSERT_TRUE(event.has_value());
    // `pending` is an order the engine has taken but not yet placed. It is an
    // acknowledgement either way.
    EXPECT_EQ(event->type, OrderEventType::Accepted);
    EXPECT_EQ(event->side, Side::Buy);
    EXPECT_EQ(event->qty.raw, 50'000'000);
    EXPECT_EQ(event->leaves_qty.raw, 50'000'000);
    EXPECT_EQ(event->price.raw, 6'350'000);
    EXPECT_EQ(event->exchange_order_id.view(), "a9625b04-fc66-4999-a876-543c3684d702");
    // The ULID is passed IN: the UUID that came back cannot be turned back
    // into one, and an event with an empty client id routes nowhere.
    EXPECT_EQ(event->exchange_client_order_id.view(), k_ulid);
}

TEST(CoinbaseMap, ARejectionAndACancellationAreBothTerminalAndDifferent) {
    const auto table = table_with_btc();
    const auto id = *ExchangeClientOrderId::from(k_ulid);

    const auto rejected_body = fixture("rest_order_rejected.json");
    const auto rejected = parse_order(rejected_body);
    ASSERT_TRUE(rejected.has_value());
    EXPECT_EQ(rejected->reject_reason, "post only");
    const auto reject_event = map_order_event(*rejected, table, id, k_ingest);
    ASSERT_TRUE(reject_event.has_value());
    EXPECT_EQ(reject_event->type, OrderEventType::Rejected);
    EXPECT_EQ(reject_event->leaves_qty.raw, 0); // never reached the book

    const auto canceled_body = fixture("rest_order_done_canceled.json");
    const auto canceled = parse_order(canceled_body);
    ASSERT_TRUE(canceled.has_value());
    const auto cancel_event = map_order_event(*canceled, table, id, k_ingest);
    ASSERT_TRUE(cancel_event.has_value());
    EXPECT_EQ(cancel_event->type, OrderEventType::Canceled);
    // 0.5 ordered, 0.2 filled: 0.3 BTC withdrawn.
    EXPECT_EQ(cancel_event->leaves_qty.raw, 30'000'000);
    EXPECT_EQ(cancel_event->ts_event.ns, parse_iso8601("2026-08-26T09:35:44.221000Z")->ns);

    // A done/filled order is described by executions this adapter has no
    // source for. Announcing a Filled here would book a quantity at a price
    // nothing on this path knows.
    const auto filled_body = fixture("rest_order_done_filled.json");
    const auto filled = parse_order(filled_body);
    ASSERT_TRUE(filled.has_value());
    EXPECT_FALSE(map_order_event(*filled, table, id, k_ingest).has_value());
    // The status REPORT still states it, because a reconciler must see it.
    const auto report = map_open_order(*filled, table, id);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->status, OrderStatus::Filled);
    EXPECT_EQ(report->leaves.raw, 0);
}

TEST(CoinbaseMap, AMarketOrderCarriesNoPriceAndItStaysUndefined) {
    const auto table = table_with_btc();
    const auto body = fixture("rest_order_market.json");
    const auto order = parse_order(body);
    ASSERT_TRUE(order.has_value());
    EXPECT_TRUE(order->price.empty());

    const auto event =
        map_order_event(*order, table, *ExchangeClientOrderId::from(k_ulid), k_ingest);
    ASSERT_TRUE(event.has_value());
    // Undefined, not zero: a zero price is a number somebody averages.
    EXPECT_TRUE(event->price.is_undef());
    EXPECT_EQ(event->qty.raw, 25'000'000);
}

TEST(CoinbaseMap, ASweepReportsPartialFillsAndDropsProductsNobodyLoaded) {
    const auto table = table_with_btc();
    const auto body = fixture("rest_orders_open.json");
    auto storage = std::make_unique<OrderBuffer>();
    const auto orders = parse_orders(body, *storage);
    ASSERT_TRUE(orders.has_value());
    ASSERT_EQ(orders->size(), 2U);

    const auto btc = map_open_order((*orders)[0], table, *ExchangeClientOrderId::from(k_ulid));
    ASSERT_TRUE(btc.has_value());
    EXPECT_EQ(btc->status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(btc->qty.raw, 50'000'000);
    EXPECT_EQ(btc->leaves.raw, 30'000'000);

    // ETH-USD is a real product this table never loaded. Dropped, never
    // invented — the sweep is account-wide and names everything we trade.
    EXPECT_FALSE(map_open_order((*orders)[1], table, ExchangeClientOrderId{}).has_value());
}

TEST(CoinbaseErrors, AnUnauthorizedBodyIsTheWholeMessage) {
    // Verbatim capture: every unauthenticated private call answers this.
    const auto body = fixture("rest_error_unauthorized.json");
    const auto message = parse_rest_error(body);
    ASSERT_TRUE(message.has_value());
    EXPECT_EQ(*message, "Unauthorized.");
    EXPECT_EQ(classify_http(401), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify_http(403), exec::ErrorClass::AuthFailure);
    // A cancel naming an order Coinbase no longer has; the adapter swallows it
    // rather than raising a CancelRejected against a closed order.
    EXPECT_EQ(classify_http(404), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify_http(429), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify_http(0), exec::ErrorClass::Connectivity);
}

} // namespace
} // namespace raikiri::venues::coinbase
