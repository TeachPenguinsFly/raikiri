#include <raikiri/venues/coinbase/coinbase_venue.hpp>
#include <raikiri/venues/coinbase/errors.hpp>
#include <raikiri/venues/coinbase/map.hpp>
#include <raikiri/venues/coinbase/rest.hpp>
#include <raikiri/venues/coinbase/wire.hpp>
#include <raikiri/venues/coinbase/ws.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
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

constexpr UnixNanos k_ingest{1'700'000'000'000'000'000ULL};

// ----------------------------------------------------------- adapter ----

TEST(CoinbaseVenue, RegistersAndBuilds) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto adapter = exec::make_venue(
        {.venue = VenueId::Coinbase, .enable_spot = true, .enable_perp = false}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Coinbase);
    EXPECT_NE(adapter->data, nullptr);
}

TEST(CoinbaseVenue, OrderEntryRefusesPermanently) {
    // Read-only venue: an order must fail loudly, never wait for a verdict
    // that no code will ever produce.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue(
        {.venue = VenueId::Coinbase, .enable_spot = true, .enable_perp = false}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.client_order_id = ClientOrderId::make(1, 42);
    adapter->execution->submit(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);

    // Reconciliation pulls answer empty rather than failing: there is nothing
    // of ours here, which is different from being unable to ask.
    EXPECT_TRUE(adapter->execution->fill_reports(MarketType::Perp, UnixNanos{0}).has_value());
}

TEST(CoinbaseVenue, PerpCatalogueIsRefusedNotEmptied) {
    // Coinbase Exchange lists no derivatives. An empty vector would read to a
    // caller as a venue having a quiet day; only an error says "wrong market".
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue(
        {.venue = VenueId::Coinbase, .enable_spot = true, .enable_perp = false}, sink, clock);
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
    const auto adapter = exec::make_venue(
        {.venue = VenueId::Coinbase, .enable_spot = true, .enable_perp = true}, sink, clock);
    ASSERT_FALSE(adapter.has_value());
    EXPECT_EQ(adapter.error(), exec::MakeError::UnsupportedMarket);
}

// -------------------------------------------------------------- wire ----

TEST(CoinbaseWire, EventDispatchResolvesKindAndProduct) {
    for (const auto& [file, kind] : {std::pair{"ws_ticker.json", EventKind::Ticker},
                                     std::pair{"ws_match.json", EventKind::Match},
                                     std::pair{"ws_last_match.json", EventKind::Match},
                                     std::pair{"ws_subscriptions.json", EventKind::Subscriptions},
                                     std::pair{"ws_error.json", EventKind::Error},
                                     // A depth frame is not subscribed to and must fall through to
                                     // Unknown rather than reaching a parser that would refuse it.
                                     std::pair{"ws_l2update.json", EventKind::Unknown}}) {
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

// ---------------------------------------------------------------- ws ----

TEST(CoinbaseWs, ChannelsCoverExactlyWhatThisVenueStreams) {
    EXPECT_EQ(channel_for(exec::SubKind::Bbo), "ticker");
    EXPECT_EQ(channel_for(exec::SubKind::Trades), "matches");
    // Funding is empty because spot HAS no funding — the DataClient default
    // then declines, which is the honest answer rather than a faked zero.
    EXPECT_TRUE(channel_for(exec::SubKind::Funding).empty());
    EXPECT_TRUE(channel_for(exec::SubKind::MarkPrice).empty());
    // Depth: the only level-2 channel opens with the whole book, over the
    // transport's frame ceiling. See ws.hpp.
    EXPECT_TRUE(channel_for(exec::SubKind::Depth).empty());
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

} // namespace
} // namespace raikiri::venues::coinbase
