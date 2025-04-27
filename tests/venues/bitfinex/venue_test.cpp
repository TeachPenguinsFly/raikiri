#include <raikiri/venues/bitfinex/bitfinex_venue.hpp>
#include <raikiri/venues/bitfinex/errors.hpp>
#include <raikiri/venues/bitfinex/map.hpp>
#include <raikiri/venues/bitfinex/rest.hpp>
#include <raikiri/venues/bitfinex/ws.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>

namespace raikiri::venues::bitfinex {
namespace {

// Returned BY REFERENCE and cached for the life of the process. Every view a
// parser hands back points into the buffer it was given, so a fixture that
// died at the end of the call expression would leave every assertion below
// reading freed memory. That lifetime contract is the design, not an
// inconvenience — it is what lets the hot path parse in place.
const std::string& fixture(std::string_view name) {
    static std::map<std::string, std::string, std::less<>> cache;
    if (const auto it = cache.find(name); it != cache.end()) {
        return it->second;
    }
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return cache.emplace(std::string{name}, buf.str()).first->second;
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

// The two instruments every capture in fixtures/ was taken from, built through
// the real mapper so the table under test carries the derived 1e-8 grid rather
// than one the test invented.
exec::InstrumentTable table_with_btc() {
    exec::InstrumentTable table;
    const auto spot =
        map_instrument({.pair = "BTCUSD", .min_order_size = "0.00004"}, MarketType::Spot);
    EXPECT_TRUE(spot.has_value());
    EXPECT_TRUE(table.add(*spot).has_value());
    const auto perp =
        map_instrument({.pair = "BTCF0:USTF0", .min_order_size = "0.00004"}, MarketType::Perp);
    EXPECT_TRUE(perp.has_value());
    EXPECT_TRUE(table.add(*perp).has_value());
    return table;
}

constexpr UnixNanos k_ingest{1'700'000'000'000'000'000ULL};

// --------------------------------------------------------------- venue ----

TEST(BitfinexVenue, RegistersAndBuilds) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto adapter =
        exec::make_venue({.venue = VenueId::Bitfinex, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Bitfinex);
    EXPECT_NE(adapter->data, nullptr);
}

TEST(BitfinexVenue, OrderWithoutAnImplementationFailsLoudly) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Bitfinex, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.client_order_id = ClientOrderId::make(1, 42);
    adapter->execution->submit(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);
}

// ---------------------------------------------------------- dispatch ----

TEST(BitfinexWire, FrameDispatchSeparatesControlFromData) {
    for (const auto& [file, kind] : {std::pair{"ws_info.json", FrameKind::Info},
                                     std::pair{"ws_subscribed_ticker.json", FrameKind::Subscribed},
                                     std::pair{"ws_error_symbol.json", FrameKind::Error},
                                     std::pair{"ws_pong.json", FrameKind::Pong},
                                     std::pair{"ws_ticker.json", FrameKind::Data},
                                     std::pair{"ws_trade_te.json", FrameKind::Data},
                                     std::pair{"ws_book_update.json", FrameKind::Data},
                                     std::pair{"ws_deriv_status.json", FrameKind::Data},
                                     std::pair{"ws_heartbeat.json", FrameKind::Heartbeat}}) {
        const auto frame = fixture(file);
        const auto info = parse_frame(frame);
        ASSERT_TRUE(info.has_value()) << file;
        EXPECT_EQ(info->kind, kind) << file;
    }
}

TEST(BitfinexWire, HeartbeatIsNotAPayload) {
    // "hb" sits in the same slot a trade tag does. A dispatcher that does not
    // single it out hands it to a payload parser, and a payload parser that
    // accepts it publishes a price of nothing.
    const auto frame = fixture("ws_heartbeat.json");
    const auto info = parse_frame(frame);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->kind, FrameKind::Heartbeat);
    EXPECT_EQ(info->chan_id, 862367);
    EXPECT_FALSE(parse_ticker(frame).has_value());
    TradeBuffer buffer;
    EXPECT_FALSE(parse_trades(frame, buffer).has_value());
    EXPECT_FALSE(parse_deriv_status(frame).has_value());
}

TEST(BitfinexWire, EveryDataFrameCarriesOnlyAChannelId) {
    // The point of the registry: nothing in a data frame names an instrument.
    for (const auto& [file, chan] : {std::pair{"ws_ticker.json", 862367},
                                     std::pair{"ws_trade_te.json", 694830},
                                     std::pair{"ws_book_snapshot.json", 722232},
                                     std::pair{"ws_deriv_status.json", 782668}}) {
        const auto frame = fixture(file);
        const auto info = parse_frame(frame);
        ASSERT_TRUE(info.has_value()) << file;
        EXPECT_EQ(info->chan_id, chan) << file;
        EXPECT_EQ(frame.find("BTC"), std::string::npos) << file;
    }
}

TEST(BitfinexWire, SubscribeAckIsTheOnlyBindingOfIdToInstrument) {
    struct Expected {
        const char* file;
        std::int64_t chan_id;
        ChannelKind kind;
        const char* symbol;
    };

    for (const auto& e :
         {Expected{"ws_subscribed_ticker.json", 862367, ChannelKind::Ticker, "tBTCUSD"},
          Expected{"ws_subscribed_trades.json", 694830, ChannelKind::Trades, "tBTCUSD"},
          Expected{"ws_subscribed_book.json", 722232, ChannelKind::Book, "tBTCUSD"},
          Expected{
              "ws_subscribed_ticker_perp.json", 840202, ChannelKind::Ticker, "tBTCF0:USTF0"}}) {
        const auto frame = fixture(e.file);
        const auto ack = parse_subscribed(frame);
        ASSERT_TRUE(ack.has_value()) << e.file;
        EXPECT_EQ(ack->chan_id, e.chan_id) << e.file;
        EXPECT_EQ(channel_kind(ack->channel, ack->key), e.kind) << e.file;
        EXPECT_EQ(ack->symbol, e.symbol) << e.file;
    }
}

TEST(BitfinexWire, DerivStatusAckNamesItsInstrumentOnlyInsideTheKey) {
    // This ack carries no `symbol` field at all; a reader that only looks
    // there binds a channel to nothing and drops every funding frame.
    const auto frame = fixture("ws_subscribed_status.json");
    const auto ack = parse_subscribed(frame);
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->chan_id, 782668);
    EXPECT_TRUE(ack->symbol.empty());
    EXPECT_EQ(ack->key, "deriv:tBTCF0:USTF0");
    EXPECT_EQ(channel_kind(ack->channel, ack->key), ChannelKind::DerivStatus);
    EXPECT_EQ(symbol_from_deriv_key(ack->key), "tBTCF0:USTF0");
}

TEST(BitfinexWire, ChannelIdsDifferPerConnection) {
    // Four captures, four sockets, four different ids for the same pair — the
    // reason the registry cannot survive a reconnect.
    const auto a = parse_subscribed(fixture("ws_subscribed_ticker.json"));
    const auto b = parse_subscribed(fixture("ws_subscribed_trades.json"));
    const auto c = parse_subscribed(fixture("ws_subscribed_book.json"));
    ASSERT_TRUE(a.has_value() && b.has_value() && c.has_value());
    EXPECT_EQ(a->symbol, b->symbol);
    EXPECT_EQ(b->symbol, c->symbol);
    EXPECT_NE(a->chan_id, b->chan_id);
    EXPECT_NE(b->chan_id, c->chan_id);
}

// ------------------------------------------------------------ ticker ----

TEST(BitfinexWire, TickerFieldsAreReadAtTheirDeclaredPositions) {
    const auto frame = fixture("ws_ticker.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    // Spellings, not values: the venue's exact token is what the exponent
    // parser downstream has to cope with.
    EXPECT_EQ(wire->bid, "63947");
    EXPECT_EQ(wire->bid_size, "2.1873936");
    EXPECT_EQ(wire->ask, "63948");
    EXPECT_EQ(wire->ask_size, "2.05831129");
    EXPECT_EQ(wire->last_price, "63953");
}

TEST(BitfinexWire, TickerArrivesWithElevenFieldsWhereTenAreDocumented) {
    // Pinned deliberately. Position IS the schema here, so the element count
    // is the only structural signal the format offers; requiring exactly the
    // documented ten would refuse every live frame, and not watching the count
    // at all would let an append go by unseen.
    const auto frame = fixture("ws_ticker.json");
    const auto payload = frame.substr(frame.find('[', 1));
    EXPECT_EQ(std::count(payload.begin(), payload.end(), ',') + 1, 11);
    EXPECT_GE(11U, k_ticker_arity);
    EXPECT_TRUE(parse_ticker(frame).has_value());
}

TEST(BitfinexWire, TickerBecomesTopOfBook) {
    const auto table = table_with_btc();
    const auto wire = parse_ticker(fixture("ws_ticker.json"));
    ASSERT_TRUE(wire.has_value());
    const auto bbo = map_bbo(*wire, "tBTCUSD", table, UnixNanos{0}, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 6'394'700'000'000); // 63947 / 1e-8
    EXPECT_EQ(bbo->ask.raw, 6'394'800'000'000);
    EXPECT_EQ(bbo->bid_qty.raw, 218'739'360); // 2.1873936 / 1e-8
    EXPECT_EQ(bbo->ask_qty.raw, 205'831'129);
    // No time in the ticker payload and no envelope timestamp on this capture,
    // so ingest is the only honest answer.
    EXPECT_EQ(bbo->ts_event.ns, k_ingest.ns);
    EXPECT_EQ(bbo->ts_init.ns, k_ingest.ns);
}

TEST(BitfinexMap, OneSlotShiftInTheTickerIsRefused) {
    // The whole reason the mapper asserts an invariant the parser cannot. A
    // reader off by one slot puts BID_SIZE where ASK belongs; every token is
    // still a valid number and nothing upstream errors, so bid < ask is what
    // catches it.
    const auto table = table_with_btc();
    WireTicker shifted{};
    shifted.bid = "63947";
    shifted.bid_size = "63948"; // ask read into the size slot
    shifted.ask = "2.05831129"; // size read into the ask slot
    shifted.ask_size = "-1381";
    EXPECT_FALSE(map_bbo(shifted, "tBTCUSD", table, UnixNanos{0}, k_ingest).has_value());
}

TEST(BitfinexMap, UnboundSymbolPublishesNothing) {
    // A data frame resolves through the registry; if the registry names a pair
    // the catalogue does not carry, there is no instrument to attribute to.
    const auto table = table_with_btc();
    const auto wire = parse_ticker(fixture("ws_ticker.json"));
    ASSERT_TRUE(wire.has_value());
    EXPECT_FALSE(map_bbo(*wire, "tDOGE:USD", table, UnixNanos{0}, k_ingest).has_value());
}

// ------------------------------------------------------------ trades ----

TEST(BitfinexWire, TradeExecutedAndTradeUpdateAreTheSamePrint) {
    // `tu` restates the print `te` already delivered — same id, same time,
    // same size, same price. Publishing both doubles every volume figure.
    TradeBuffer buffer;
    const auto te = parse_trades(fixture("ws_trade_te.json"), buffer);
    ASSERT_TRUE(te.has_value());
    ASSERT_EQ(te->trades.size(), 1U);
    EXPECT_EQ(te->kind, TradeFrameKind::Executed);
    const auto executed = te->trades[0];
    EXPECT_EQ(executed.id, "1956671261");
    EXPECT_EQ(executed.ts_ms, 1'786'430'928'128ULL);
    EXPECT_EQ(executed.amount, "-0.0002");
    EXPECT_EQ(executed.price, "63951");

    TradeBuffer other;
    const auto tu = parse_trades(fixture("ws_trade_tu.json"), other);
    ASSERT_TRUE(tu.has_value());
    ASSERT_EQ(tu->trades.size(), 1U);
    EXPECT_EQ(tu->kind, TradeFrameKind::Updated);
    EXPECT_EQ(tu->trades[0].id, executed.id);
    EXPECT_EQ(tu->trades[0].price, executed.price);
    EXPECT_EQ(tu->trades[0].amount, executed.amount);
}

TEST(BitfinexWire, TradesSnapshotIsHistoryNotOnePrint) {
    TradeBuffer buffer;
    const auto wire = parse_trades(fixture("ws_trades_snapshot.json"), buffer);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->kind, TradeFrameKind::Snapshot);
    ASSERT_EQ(wire->trades.size(), 30U);
    EXPECT_EQ(wire->trades.front().id, "1956671260");
    EXPECT_EQ(wire->trades.back().id, "1956667168");
    // Four minutes of backfill in one frame, which is why it is not published.
    EXPECT_GT(wire->trades.front().ts_ms - wire->trades.back().ts_ms, 200'000ULL);
}

TEST(BitfinexMap, TradeAmountSignIsTheAggressor) {
    const auto table = table_with_btc();
    TradeBuffer buffer;
    const auto wire = parse_trades(fixture("ws_trade_te.json"), buffer);
    ASSERT_TRUE(wire.has_value());
    const auto trade = map_trade(wire->trades[0], "tBTCUSD", table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    // -0.0002 at 63951, which was the bid in the same session's ticker: the
    // taker sold. There is no side field to disagree with, so this is the only
    // place the direction is decided.
    EXPECT_EQ(trade->aggressor, Side::Sell);
    EXPECT_EQ(trade->qty.raw, 20'000); // 0.0002 / 1e-8, magnitude only
    EXPECT_EQ(trade->price.raw, 6'395'100'000'000);
    EXPECT_EQ(trade->ts_event.ns, 1'786'430'928'128ULL * 1'000'000ULL);

    WireTrade bought = wire->trades[0];
    bought.amount = "0.0002";
    const auto buy = map_trade(bought, "tBTCUSD", table, k_ingest);
    ASSERT_TRUE(buy.has_value());
    EXPECT_EQ(buy->aggressor, Side::Buy);
    EXPECT_EQ(buy->qty.raw, trade->qty.raw);
}

TEST(BitfinexMap, ScientificNotationAmountSurvives) {
    // `1e-8` on the live trades channel. parse_decimal_e9 refuses an exponent
    // outright, so without the exponent-aware parser this print vanishes.
    const auto table = table_with_btc();
    TradeBuffer buffer;
    const auto wire = parse_trades(fixture("ws_trade_te_scientific.json"), buffer);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->trades.size(), 1U);
    EXPECT_EQ(wire->trades[0].amount, "1e-8");
    const auto trade = map_trade(wire->trades[0], "tBTCUSD", table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->qty.raw, 1); // exactly one step of the derived grid
    EXPECT_EQ(trade->aggressor, Side::Buy);
}

// -------------------------------------------------------------- book ----

TEST(BitfinexWire, BookSnapshotAndDeltaShareOneShape) {
    LevelBuffer buffer;
    const auto snapshot = parse_book(fixture("ws_book_snapshot.json"), buffer);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_TRUE(snapshot->snapshot);
    ASSERT_EQ(snapshot->levels.size(), 50U); // 25 a side at len=25
    EXPECT_EQ(snapshot->levels.front().price, "63951");
    EXPECT_EQ(snapshot->levels.front().count, 1);
    EXPECT_EQ(snapshot->levels.front().amount, "0.00794427");

    LevelBuffer other;
    const auto update = parse_book(fixture("ws_book_update.json"), other);
    ASSERT_TRUE(update.has_value());
    // No tag distinguishes these two; only the type of the payload's first
    // element does.
    EXPECT_FALSE(update->snapshot);
    ASSERT_EQ(update->levels.size(), 1U);
    EXPECT_EQ(update->levels[0].price, "63950");
    EXPECT_EQ(update->levels[0].count, 1);
    EXPECT_EQ(update->levels[0].amount, "0.0002");
}

TEST(BitfinexWire, BookAmountSignIsTheSideNotTheAggressor) {
    // Same field name, different meaning from the trades channel: here the
    // sign says which book the level belongs to, and a zero count removes it.
    LevelBuffer bid_buffer;
    const auto bid = parse_book(fixture("ws_book_delete.json"), bid_buffer);
    ASSERT_TRUE(bid.has_value());
    ASSERT_EQ(bid->levels.size(), 1U);
    EXPECT_EQ(bid->levels[0].count, 0);
    EXPECT_EQ(bid->levels[0].amount, "1");

    LevelBuffer ask_buffer;
    const auto ask = parse_book(fixture("ws_book_delete_ask.json"), ask_buffer);
    ASSERT_TRUE(ask.has_value());
    ASSERT_EQ(ask->levels.size(), 1U);
    EXPECT_EQ(ask->levels[0].count, 0);
    EXPECT_EQ(ask->levels[0].amount, "-1");
}

// ------------------------------------------------------ deriv status ----

TEST(BitfinexWire, DerivStatusReadsPastFifteenNullPlaceholders) {
    const auto wire = parse_deriv_status(fixture("ws_deriv_status.json"));
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->ts_ms, 1'786'430'959'000ULL);
    EXPECT_EQ(wire->deriv_price, "63969.45042711");
    EXPECT_EQ(wire->spot_price, "63938");
    EXPECT_EQ(wire->next_funding_ms, 1'786'435'200'000ULL);
    EXPECT_EQ(wire->next_funding_accrued, "0.00044005");
    EXPECT_EQ(wire->current_funding, "0.00014161");
    EXPECT_EQ(wire->mark_price, "63939.3664");
    EXPECT_EQ(wire->open_interest, "8874.69406153");
    // Slot 14 and slot 2 are thirty dollars apart on BTC. Reading one for the
    // other is the mistake this layout invites and nothing would flag.
    EXPECT_NE(wire->mark_price, wire->deriv_price);
}

TEST(BitfinexMap, FundingCarriesMarkNotDerivPrice) {
    const auto table = table_with_btc();
    const auto wire = parse_deriv_status(fixture("ws_deriv_status.json"));
    ASSERT_TRUE(wire.has_value());
    const auto funding = map_funding(*wire, "tBTCF0:USTF0", table, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 141'610);                 // 0.00014161
    EXPECT_EQ(funding->predicted_rate.raw, 440'050);       // 0.00044005
    EXPECT_EQ(funding->mark_price.raw, 6'393'936'640'000); // 63939.3664 / 1e-8
    EXPECT_EQ(funding->next_funding_time.ns, 1'786'435'200'000ULL * 1'000'000ULL);
    EXPECT_EQ(funding->interval_s, k_funding_interval_s);
    EXPECT_EQ(funding->ts_event.ns, 1'786'430'959'000ULL * 1'000'000ULL);
}

TEST(BitfinexMap, FundingSettlementLandsOnAnEightHourBoundary) {
    // The interval is a constant because the payload states one settlement
    // time and not two, so no gap can be measured. This is the evidence for
    // the constant being 8h rather than a guess.
    const auto wire = parse_deriv_status(fixture("ws_deriv_status.json"));
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ((wire->next_funding_ms / 1000) % k_funding_interval_s, 0U);
}

TEST(BitfinexMap, NoRateIsNotAZeroRate) {
    const auto table = table_with_btc();
    auto wire = *parse_deriv_status(fixture("ws_deriv_status.json"));
    wire.current_funding = {};
    EXPECT_FALSE(map_funding(wire, "tBTCF0:USTF0", table, k_ingest).has_value());
}

// ----------------------------------------- positional safety, negative ----

TEST(BitfinexWire, ShortPayloadIsAnErrorNotAPartialRead) {
    // Truncation is the one structural failure a positional wire can signal.
    // A partly-filled struct here would publish a plausible zero.
    EXPECT_FALSE(parse_ticker("[862367,[63947,2.1873936,63948]]").has_value());
    TradeBuffer buffer;
    EXPECT_FALSE(parse_trades(R"([694830,"te",[1956671261,1786430928128]])", buffer).has_value());
    EXPECT_FALSE(parse_deriv_status("[782668,[1786430959000,null,63969.45]]").has_value());
    LevelBuffer levels;
    EXPECT_FALSE(parse_book("[722232,[63950,1]]", levels).has_value());
    EXPECT_FALSE(parse_frame("[862367]").has_value());
}

TEST(BitfinexWire, ShortPayloadReportsWhichFailureItWas) {
    const auto err = parse_ticker("[862367,[63947,2.1873936,63948]]");
    ASSERT_FALSE(err.has_value());
    EXPECT_EQ(err.error(), WireError::ShortArray);
    EXPECT_EQ(to_string(WireError::ShortArray), "ShortArray");
}

TEST(BitfinexWire, TrailingFieldsAreIgnoredRatherThanRefused) {
    // Bitfinex already appends past its own documentation, and the `conf`
    // TIMESTAMP flag appends again. Growth at the tail renames nothing, so it
    // must not break a reader.
    const auto plain = parse_ticker(fixture("ws_ticker.json"));
    const auto stamped = parse_ticker(fixture("ws_ticker_timestamped.json"));
    ASSERT_TRUE(plain.has_value());
    ASSERT_TRUE(stamped.has_value());
    EXPECT_EQ(stamped->bid, "63914");
    EXPECT_EQ(stamped->ask, "63918");
}

TEST(BitfinexWire, EnvelopeTimestampIsReadWithoutParsingThePayload) {
    // The ticker channel has no time of its own; this is the only one it gets.
    const auto stamped = parse_frame(fixture("ws_ticker_timestamped.json"));
    ASSERT_TRUE(stamped.has_value());
    EXPECT_EQ(stamped->kind, FrameKind::Data);
    EXPECT_EQ(stamped->chan_id, 707879);
    EXPECT_EQ(stamped->ts_ms, 1'786'431'364'643ULL);

    const auto plain = parse_frame(fixture("ws_ticker.json"));
    ASSERT_TRUE(plain.has_value());
    EXPECT_EQ(plain->ts_ms, 0ULL);
}

TEST(BitfinexWire, TaggedEnvelopeKeepsItsTimestampBehindThePayload) {
    const auto info = parse_frame(fixture("ws_trade_te_timestamped.json"));
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->kind, FrameKind::Data);
    EXPECT_EQ(info->tag, "te");
    EXPECT_EQ(info->ts_ms, 1'786'431'367'786ULL);

    const auto beat = parse_frame(fixture("ws_heartbeat_timestamped.json"));
    ASSERT_TRUE(beat.has_value());
    EXPECT_EQ(beat->kind, FrameKind::Heartbeat);
    EXPECT_EQ(beat->ts_ms, 1'786'431'367'792ULL);
}

TEST(BitfinexMap, TimestampedTickerCarriesTheVenueClock) {
    const auto table = table_with_btc();
    const auto frame = fixture("ws_ticker_timestamped.json");
    const auto info = parse_frame(frame);
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(info.has_value() && wire.has_value());
    const auto bbo =
        map_bbo(*wire, "tBTCUSD", table, ms_to_unix(info->ts_ms, UnixNanos{0}), k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->ts_event.ns, 1'786'431'364'643ULL * 1'000'000ULL);
    EXPECT_EQ(bbo->ts_init.ns, k_ingest.ns);
}

// ------------------------------------------------------------ errors ----

TEST(BitfinexWire, StreamErrorCodesAreNumbers) {
    for (const auto& file : {"ws_error_symbol.json", "ws_error_channel.json"}) {
        const auto frame = fixture(file);
        const auto info = parse_frame(frame);
        ASSERT_TRUE(info.has_value()) << file;
        EXPECT_EQ(info->kind, FrameKind::Error) << file;
        EXPECT_EQ(info->code, k_subscription_failed) << file;
        const auto err = parse_stream_error(frame);
        ASSERT_TRUE(err.has_value()) << file;
        EXPECT_EQ(err->code, k_subscription_failed) << file;
        EXPECT_FALSE(err->msg.empty()) << file;
    }
}

TEST(BitfinexWire, RestErrorIsAnArrayNotAnEnvelope) {
    const auto body = fixture("rest_error.json");
    EXPECT_TRUE(is_rest_error(body));
    const auto err = parse_rest_error(body);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, k_request_parameters);
    EXPECT_EQ(err->msg, "symbol: invalid");
    // Data bodies start with a number or a nested array, never a string.
    EXPECT_FALSE(is_rest_error(fixture("rest_platform_status.json")));
    EXPECT_FALSE(is_rest_error(fixture("rest_conf_pair.json")));
    EXPECT_FALSE(is_rest_error(fixture("rest_ticker.json")));
}

TEST(BitfinexErrors, ClockSkewIsRetryableAndUnknownIsTerminal) {
    EXPECT_EQ(classify(k_auth_nonce), exec::ErrorClass::Retryable);
    EXPECT_NE(classify(k_auth_nonce), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify(k_maintenance_end), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_server_stopping), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_auth_signature), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify(k_rate_limit), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(k_subscription_failed), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_channel_limit), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(999'999), exec::ErrorClass::Terminal);
}

// --------------------------------------------------------- catalogue ----

TEST(BitfinexWire, PairInfoIsNestedThreeDeep) {
    static PairBuffer storage;
    const auto rows = parse_pair_info(fixture("rest_conf_pair.json"), storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);
    EXPECT_EQ((*rows)[0].pair, "BTCUSD"); // no `t` prefix on this endpoint
    EXPECT_EQ((*rows)[0].min_order_size, "0.00004");
    EXPECT_EQ((*rows)[0].max_order_size, "2000.0");
    // A row where the margin slots beside the sizes are null, so the reader
    // cannot lean on "the tail is always numeric".
    EXPECT_EQ((*rows)[2].pair, "AAVE:USD");
    EXPECT_EQ((*rows)[2].min_order_size, "0.02");
}

TEST(BitfinexWire, PairInfoArityDiffersBetweenMarkets) {
    // 12 slots on spot, 10 on futures. Only the prefix the mapper reads is
    // required, which is why one parser serves both.
    static PairBuffer storage;
    const auto rows = parse_pair_info(fixture("rest_conf_pair_futures.json"), storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);
    EXPECT_EQ((*rows)[0].pair, "BTCF0:USTF0");
    EXPECT_EQ((*rows)[0].min_order_size, "0.00004");
    EXPECT_EQ((*rows)[2].pair, "AUSTRALIA200IXF0:USTF0");
}

TEST(BitfinexMap, PaperTradingPairsAreRefused) {
    // TEST-prefixed pairs are served from the same production endpoint as the
    // real ones and quote simulated prices; 28 of 272 rows. Admitting one puts
    // a fabricated book in a live catalogue.
    EXPECT_FALSE(
        map_instrument({.pair = "TESTBTC:TESTUSD", .min_order_size = "0.00004"}, MarketType::Spot)
            .has_value());
    EXPECT_FALSE(map_instrument({.pair = "TESTBTCF0:TESTUSDTF0", .min_order_size = "0.00004"},
                                MarketType::Perp)
                     .has_value());
}

TEST(BitfinexMap, CatalogueLoadsBothMarketsAndDropsThePaperRows) {
    static PairBuffer storage;
    for (const auto& [file, market, first] :
         {std::tuple{"rest_conf_pair.json", MarketType::Spot, "tBTCUSD"},
          std::tuple{"rest_conf_pair_futures.json", MarketType::Perp, "tBTCF0:USTF0"}}) {
        const auto rows = parse_pair_info(fixture(file), storage);
        ASSERT_TRUE(rows.has_value()) << file;
        std::vector<Instrument> loaded;
        for (const auto& row : *rows) {
            if (auto instrument = map_instrument(row, market); instrument.has_value()) {
                loaded.push_back(*instrument);
            }
        }
        ASSERT_EQ(loaded.size(), 3U) << file; // four rows, one of them paper
        EXPECT_EQ(base_of(loaded.front()).exchange_symbol.view(), first) << file;
        EXPECT_EQ(base_of(loaded.front()).id.market(), market) << file;
    }
}

TEST(BitfinexMap, MarketIsDecidedByTheSymbolShape) {
    // Loading a perpetual as spot is a whole category of position bug, and the
    // F0 suffix is the venue's own statement of which is which.
    EXPECT_FALSE(
        map_instrument({.pair = "BTCF0:USTF0", .min_order_size = "0.00004"}, MarketType::Spot)
            .has_value());
    EXPECT_FALSE(map_instrument({.pair = "BTCUSD", .min_order_size = "0.00004"}, MarketType::Perp)
                     .has_value());
    EXPECT_FALSE(map_instrument({.pair = "BTCUSD", .min_order_size = "0.00004"}, MarketType::Future)
                     .has_value());
}

TEST(BitfinexMap, EveryInstrumentCarriesTheSameDerivedGrid) {
    // No tick and no lot are published anywhere, so both are the eight-decimal
    // cap. If Bitfinex ever states a real per-pair step, this is the assertion
    // that has to change.
    const auto spot =
        map_instrument({.pair = "BTCUSD", .min_order_size = "0.00004"}, MarketType::Spot);
    ASSERT_TRUE(spot.has_value());
    const auto& base = base_of(*spot);
    EXPECT_EQ(base.tick_size.raw, k_step_e9);
    EXPECT_EQ(base.lot_size.raw, k_step_e9);
    EXPECT_EQ(base.contract_value.raw, k_e9); // base units, not contracts
    EXPECT_TRUE(base.min_notional.is_undef());
    EXPECT_EQ(base.exchange_symbol.view(), "tBTCUSD");

    const auto perp =
        map_instrument({.pair = "BTCF0:USTF0", .min_order_size = "0.00004"}, MarketType::Perp);
    ASSERT_TRUE(perp.has_value());
    EXPECT_EQ(base_of(*perp).tick_size.raw, k_step_e9);
    EXPECT_EQ(std::get<PerpInstrument>(*perp).funding_interval_s, k_funding_interval_s);
}

TEST(BitfinexMap, RowWhoseOnlyNumberWillNotReadIsRefused) {
    EXPECT_FALSE(
        map_instrument({.pair = "BTCUSD", .min_order_size = ""}, MarketType::Spot).has_value());
    EXPECT_FALSE(
        map_instrument({.pair = "BTCUSD", .min_order_size = "0"}, MarketType::Spot).has_value());
    EXPECT_FALSE(
        map_instrument({.pair = "BTCUSD", .min_order_size = "abc"}, MarketType::Spot).has_value());
}

TEST(BitfinexWire, PlatformStatusGatesTheCatalogue) {
    const auto status = parse_platform_status(fixture("rest_platform_status.json"));
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, k_platform_operative);
    EXPECT_EQ(parse_platform_status("[0]").value(), 0);
    EXPECT_FALSE(parse_platform_status("[]").has_value());
}

// ------------------------------------------------------------ number ----

TEST(BitfinexMap, ExponentAwareParserIsExactOrRefuses) {
    EXPECT_EQ(parse_number_e9("63947")->raw, 63'947'000'000'000);
    EXPECT_EQ(parse_number_e9("2.1873936")->raw, 2'187'393'600);
    EXPECT_EQ(parse_number_e9("1e-8")->raw, 10);
    EXPECT_EQ(parse_number_e9("-0.0002")->raw, -200'000);
    EXPECT_EQ(parse_number_e9("8.5e-05")->raw, 85'000);
    EXPECT_EQ(parse_number_e9("1E+3")->raw, 1'000'000'000'000);
    // Past what 1e9 fixed point models. Truncating silently is how a size
    // becomes a different size.
    EXPECT_FALSE(parse_number_e9("1e-10").has_value());
    EXPECT_FALSE(parse_number_e9("0.0000000001").has_value());
    EXPECT_FALSE(parse_number_e9("").has_value());
    EXPECT_FALSE(parse_number_e9("1e").has_value());
    EXPECT_FALSE(parse_number_e9("--1").has_value());
    EXPECT_FALSE(parse_number_e9("1.2.3").has_value());
}

TEST(BitfinexMap, PriceFinerThanTheGridIsDroppedNotRounded) {
    // The derived grid is 1e-8. A ninth decimal means the venue published
    // precision this adapter does not model, and rounding it would put an
    // order somewhere other than where it was meant.
    const auto table = table_with_btc();
    WireTicker wire{};
    wire.bid = "63947.000000001";
    wire.bid_size = "1";
    wire.ask = "63948";
    wire.ask_size = "1";
    EXPECT_FALSE(map_bbo(wire, "tBTCUSD", table, UnixNanos{0}, k_ingest).has_value());
}

TEST(BitfinexMap, EveryPriceTheVenueActuallySendsDividesExactly) {
    // The claim the derived grid rests on: across the spellings observed live,
    // exact division never refuses a real quote.
    const auto table = table_with_btc();
    for (const auto* token : {"63947",
                              "1873.7",
                              "0.18767",
                              "1.0052",
                              "0.070163",
                              "0.00000449",
                              "0.00070516",
                              "411.17199071"}) {
        EXPECT_TRUE(parse_number_e9(token).has_value()) << token;
        EXPECT_EQ(parse_number_e9(token)->raw % k_step_e9, 0) << token;
    }
    EXPECT_EQ(table.size(), 2U);
}

// ---------------------------------------------------------------- ws ----

TEST(BitfinexWs, EachChannelTakesItsOwnKeys) {
    Protocol protocol;
    const auto ticker =
        protocol.subscribe_frame({.kind = ChannelKind::Ticker, .symbol = "tBTCUSD"});
    ASSERT_TRUE(ticker.has_value());
    EXPECT_EQ(ticker->text, R"({"event":"subscribe","channel":"ticker","symbol":"tBTCUSD"})");

    const auto trades =
        protocol.subscribe_frame({.kind = ChannelKind::Trades, .symbol = "tBTCUSD"});
    ASSERT_TRUE(trades.has_value());
    EXPECT_EQ(trades->text, R"({"event":"subscribe","channel":"trades","symbol":"tBTCUSD"})");

    const auto book = protocol.subscribe_frame({.kind = ChannelKind::Book, .symbol = "tBTCUSD"});
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(
        book->text,
        R"({"event":"subscribe","channel":"book","symbol":"tBTCUSD","prec":"P0","freq":"F0","len":"25"})");

    // No `symbol` field at all on this one.
    const auto status =
        protocol.subscribe_frame({.kind = ChannelKind::DerivStatus, .symbol = "tBTCF0:USTF0"});
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->text,
              R"({"event":"subscribe","channel":"status","key":"deriv:tBTCF0:USTF0"})");
}

TEST(BitfinexWs, UnsubscribeIsByChannelIdBecauseThatIsTheIdentity) {
    Protocol protocol;
    const auto frame = protocol.unsubscribe_frame(862367);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->text, R"({"event":"unsubscribe","chanId":862367})");
    EXPECT_FALSE(protocol.unsubscribe_frame(0).has_value());
}

TEST(BitfinexWs, SubscriptionIntentsResolveToChannels) {
    EXPECT_EQ(kind_for(exec::SubKind::Bbo), ChannelKind::Ticker);
    EXPECT_EQ(kind_for(exec::SubKind::Trades), ChannelKind::Trades);
    EXPECT_EQ(kind_for(exec::SubKind::Depth), ChannelKind::Book);
    // Funding and mark are one channel here, not two.
    EXPECT_EQ(kind_for(exec::SubKind::Funding), ChannelKind::DerivStatus);
    EXPECT_EQ(kind_for(exec::SubKind::MarkPrice), ChannelKind::DerivStatus);
    EXPECT_EQ(kind_for(exec::SubKind::Kline), ChannelKind::Unknown);

    Protocol protocol;
    EXPECT_FALSE(
        protocol.subscribe_frame({.kind = ChannelKind::Unknown, .symbol = "tBTCUSD"}).has_value());
    EXPECT_FALSE(protocol.subscribe_frame({.kind = ChannelKind::Ticker, .symbol = {}}).has_value());
}

TEST(BitfinexWs, ConfAndPingMatchWhatTheVenueAcknowledged) {
    EXPECT_EQ(Protocol::conf_frame(), R"({"event":"conf","flags":32768})");
    EXPECT_EQ(Protocol::ping_frame(), R"({"event":"ping"})");
    const auto ack = parse_frame(fixture("ws_conf_ack.json"));
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->kind, FrameKind::Unknown); // acknowledged, and nothing acts on it
    const auto pong = parse_frame(fixture("ws_pong.json"));
    ASSERT_TRUE(pong.has_value());
    EXPECT_EQ(pong->kind, FrameKind::Pong);
}

TEST(BitfinexWs, MaintenanceNoticesAreCodedInfoFramesNotErrors) {
    // 20051 and 20061 reissue every channel id, so they have to be told apart
    // from the plain greeting, which carries no code at all.
    const auto greeting = parse_frame(fixture("ws_info.json"));
    ASSERT_TRUE(greeting.has_value());
    EXPECT_EQ(greeting->kind, FrameKind::Info);
    EXPECT_EQ(greeting->code, 0);

    const auto notice = parse_frame(R"({"event":"info","code":20061,"msg":"maintenance ended"})");
    ASSERT_TRUE(notice.has_value());
    EXPECT_EQ(notice->kind, FrameKind::Info);
    EXPECT_EQ(notice->code, k_info_maintenance_end);
}

// -------------------------------------------------------------- rest ----

TEST(BitfinexRest, PublicReadsTargetTheirEndpoints) {
    RequestBuilder builder;
    EXPECT_EQ(rest_host(), "api-pub.bitfinex.com");
    EXPECT_EQ(builder.platform_status().target, "/v2/platform/status");
    EXPECT_EQ(builder.pair_info(false).target, "/v2/conf/pub:info:pair");
    EXPECT_EQ(builder.pair_info(true).target, "/v2/conf/pub:info:pair:futures");
    EXPECT_EQ(builder.book("tBTCUSD", 25).target, "/v2/book/tBTCUSD/P0?len=25");
    EXPECT_EQ(builder.tickers().target, "/v2/tickers?symbols=ALL");
}

} // namespace
} // namespace raikiri::venues::bitfinex
