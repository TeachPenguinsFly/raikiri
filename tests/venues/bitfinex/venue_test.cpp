#include <raikiri/config/secret.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/bitfinex/bitfinex_venue.hpp>
#include <raikiri/venues/bitfinex/book.hpp>
#include <raikiri/venues/bitfinex/catalogue.hpp>
#include <raikiri/venues/bitfinex/envelope.hpp>
#include <raikiri/venues/bitfinex/errors.hpp>
#include <raikiri/venues/bitfinex/map.hpp>
#include <raikiri/venues/bitfinex/market_data.hpp>
#include <raikiri/venues/bitfinex/orders.hpp>
#include <raikiri/venues/bitfinex/positions.hpp>
#include <raikiri/venues/bitfinex/rest.hpp>
#include <raikiri/venues/bitfinex/sign.hpp>
#include <raikiri/venues/bitfinex/ws.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

namespace raikiri::venues::bitfinex {
namespace {

// cached for the process; parsed views point into it
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

// built through the real mapper so the table carries the derived 1e-8 grid
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

TEST(BitfinexVenue, RegistersAndBuilds) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto adapter =
        exec::make_venue({.venue = VenueId::Bitfinex, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Bitfinex);
    EXPECT_NE(adapter->data, nullptr);
}

TEST(BitfinexVenue, AnUnbuildableOrderComesBackAsARejectedEvent) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter =
        exec::make_venue({.venue = VenueId::Bitfinex, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.client_order_id = ClientOrderId::make(1, 42);
    adapter->execution->submit(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);
    EXPECT_EQ(sink.events[0].leaves_qty.raw, 0);

    cmd.kind = CmdKind::Cancel;
    cmd.qty = QtyLots::from_raw(500);
    adapter->execution->cancel(cmd);
    ASSERT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(sink.events[1].type, OrderEventType::CancelRejected);
    EXPECT_EQ(sink.events[1].leaves_qty.raw, 500);
}

TEST(BitfinexVenue, UnservedMarketsRefuseWithTheirOwnName) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter =
        exec::make_venue({.venue = VenueId::Bitfinex, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    const auto options = adapter->execution->open_order_reports(MarketType::Option);
    ASSERT_FALSE(options.has_value());
    EXPECT_EQ(options.error().cls, exec::ErrorClass::Terminal);
    EXPECT_EQ(options.error().message.view(), "bitfinex options order entry");

    const auto dated = adapter->execution->fill_reports(MarketType::Future, UnixNanos{0});
    ASSERT_FALSE(dated.has_value());
    EXPECT_EQ(dated.error().message.view(), "bitfinex dated futures order entry");

    const auto query = adapter->execution->query_order(ExchangeClientOrderId{}, MarketType::Option);
    ASSERT_FALSE(query.has_value());
    EXPECT_EQ(query.error().message.view(), "bitfinex options order entry");
    EXPECT_FALSE(adapter->execution->position_reports(MarketType::Option).has_value());

    const auto spot = adapter->execution->position_reports(MarketType::Spot);
    ASSERT_FALSE(spot.has_value());
    EXPECT_EQ(spot.error().message.view(), "bitfinex spot position pull");

    EXPECT_EQ(market_refusal(MarketType::Option), BuildError::OptionsUnsupported);
    EXPECT_EQ(market_refusal(MarketType::Future), BuildError::DatedFuturesUnsupported);
    EXPECT_FALSE(market_refusal(MarketType::Spot).has_value());
    EXPECT_FALSE(market_refusal(MarketType::Perp).has_value());
}

TEST(BitfinexVenue, PullsWithoutCredentialsAnswerAuthFailure) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter =
        exec::make_venue({.venue = VenueId::Bitfinex, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    for (const auto& pull : {adapter->execution->open_order_reports(MarketType::Perp)}) {
        ASSERT_FALSE(pull.has_value());
        EXPECT_EQ(pull.error().cls, exec::ErrorClass::AuthFailure);
    }
    const auto positions = adapter->execution->position_reports(MarketType::Perp);
    ASSERT_FALSE(positions.has_value());
    EXPECT_EQ(positions.error().cls, exec::ErrorClass::AuthFailure);

    const auto fills = adapter->execution->fill_reports(MarketType::Perp, UnixNanos{0});
    ASSERT_FALSE(fills.has_value());
    EXPECT_EQ(fills.error().cls, exec::ErrorClass::AuthFailure);
}

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
    // "hb" sits in the trade tag slot
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
    // this ack has no symbol field
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
    // the same pair gets a different id on each socket
    const auto a = parse_subscribed(fixture("ws_subscribed_ticker.json"));
    const auto b = parse_subscribed(fixture("ws_subscribed_trades.json"));
    const auto c = parse_subscribed(fixture("ws_subscribed_book.json"));
    ASSERT_TRUE(a.has_value() && b.has_value() && c.has_value());
    EXPECT_EQ(a->symbol, b->symbol);
    EXPECT_EQ(b->symbol, c->symbol);
    EXPECT_NE(a->chan_id, b->chan_id);
    EXPECT_NE(b->chan_id, c->chan_id);
}

TEST(BitfinexWire, TickerFieldsAreReadAtTheirDeclaredPositions) {
    const auto frame = fixture("ws_ticker.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->bid, "63947");
    EXPECT_EQ(wire->bid_size, "2.1873936");
    EXPECT_EQ(wire->ask, "63948");
    EXPECT_EQ(wire->ask_size, "2.05831129");
    EXPECT_EQ(wire->last_price, "63953");
}

TEST(BitfinexWire, TickerArrivesWithElevenFieldsWhereTenAreDocumented) {
    // pins the observed element count
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
    // no time in the payload or envelope
    EXPECT_EQ(bbo->ts_event.ns, k_ingest.ns);
    EXPECT_EQ(bbo->ts_init.ns, k_ingest.ns);
}

TEST(BitfinexMap, TheVolumeSlotIsBaseAndNothingHereStatesTheQuoteSide) {
    const auto table = table_with_btc();
    const auto frame = fixture("ws_ticker.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    // slot 7, between last price and high
    EXPECT_EQ(wire->volume, "411.13963496");

    const auto stats = map_ticker_stats(*wire, "tBTCUSD", table, UnixNanos{0}, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base.raw, 411'139'634'960);
    // zero: the venue states no quote turnover
    EXPECT_EQ(stats->volume_24h_quote.raw, 0);
    EXPECT_EQ(stats->ts_event.ns, k_ingest.ns);
    EXPECT_EQ(stats->ts_init.ns, k_ingest.ns);
}

TEST(BitfinexMap, VolumeForAnUnboundSymbolPublishesNothing) {
    const auto table = table_with_btc();
    const auto frame = fixture("ws_ticker.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_FALSE(map_ticker_stats(*wire, "tDOGE:USD", table, UnixNanos{0}, k_ingest).has_value());
}

TEST(BitfinexMap, OneSlotShiftInTheTickerIsRefused) {
    // a one-slot shift parses cleanly; bid < ask catches it
    const auto table = table_with_btc();
    WireTicker shifted{};
    shifted.bid = "63947";
    shifted.bid_size = "63948"; // ask read into the size slot
    shifted.ask = "2.05831129"; // size read into the ask slot
    shifted.ask_size = "-1381";
    EXPECT_FALSE(map_bbo(shifted, "tBTCUSD", table, UnixNanos{0}, k_ingest).has_value());
}

TEST(BitfinexMap, UnboundSymbolPublishesNothing) {
    const auto table = table_with_btc();
    const auto wire = parse_ticker(fixture("ws_ticker.json"));
    ASSERT_TRUE(wire.has_value());
    EXPECT_FALSE(map_bbo(*wire, "tDOGE:USD", table, UnixNanos{0}, k_ingest).has_value());
}

TEST(BitfinexWire, TradeExecutedAndTradeUpdateAreTheSamePrint) {
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
    // minutes of backfill; not published
    EXPECT_GT(wire->trades.front().ts_ms - wire->trades.back().ts_ms, 200'000ULL);
}

TEST(BitfinexMap, TradeAmountSignIsTheAggressor) {
    const auto table = table_with_btc();
    TradeBuffer buffer;
    const auto wire = parse_trades(fixture("ws_trade_te.json"), buffer);
    ASSERT_TRUE(wire.has_value());
    const auto trade = map_trade(wire->trades[0], "tBTCUSD", table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    // -0.0002 at the bid: the taker sold
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
    // 1e-8 as sent on the live trades channel
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
    // only the payload's first element type separates snapshot from update
    EXPECT_FALSE(update->snapshot);
    ASSERT_EQ(update->levels.size(), 1U);
    EXPECT_EQ(update->levels[0].price, "63950");
    EXPECT_EQ(update->levels[0].count, 1);
    EXPECT_EQ(update->levels[0].amount, "0.0002");
}

TEST(BitfinexWire, BookAmountSignIsTheSideNotTheAggressor) {
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

// the parsed levels point into this buffer
struct BookFrame {
    std::unique_ptr<LevelBuffer> storage{std::make_unique<LevelBuffer>()};
};

WireBook book_of(BookFrame& frame, std::string_view name) {
    const auto wire = parse_book(fixture(name), *frame.storage);
    EXPECT_TRUE(wire.has_value());
    return wire.value_or(WireBook{});
}

Book book_for(const exec::InstrumentTable& table, std::string_view symbol) {
    const auto* instrument = table.find(symbol);
    EXPECT_NE(instrument, nullptr);
    const auto& base = base_of(*instrument);
    return Book{base.id, base.tick_size, base.lot_size};
}

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

TEST(BitfinexBook, ADeltaBeforeTheSnapshotPublishesNothing) {
    const auto table = table_with_btc();
    auto book = book_for(table, "tBTCUSD");
    BookFrame update;
    EXPECT_EQ(book.apply(book_of(update, "ws_book_update.json")), BookResult::Ignored);
    EXPECT_FALSE(book.synced());
    EXPECT_FALSE(book.snapshot(k_ingest, k_ingest).has_value());
    EXPECT_FALSE(book.touch(k_ingest, k_ingest).has_value());
}

TEST(BitfinexBook, SnapshotAndUpdateMakeABookSharedMemoryCanTake) {
    const auto table = table_with_btc();
    auto book = book_for(table, "tBTCUSD");
    BookFrame snapshot_frame;
    BookFrame update;
    EXPECT_EQ(book.apply(book_of(snapshot_frame, "ws_book_snapshot.json")), BookResult::Resynced);

    const auto out = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(out.has_value());
    expect_coherent(*out);
    EXPECT_EQ(out->bid_count, 25U);
    EXPECT_EQ(out->ask_count, 25U);
    EXPECT_EQ(out->bids[0].price.raw, 6'395'100'000'000); // 63951 over the 1e-8 grid
    EXPECT_EQ(out->asks[0].price.raw, 6'395'300'000'000);
    EXPECT_EQ(out->bids[0].qty.raw, 794'427); // 0.00794427, its sign dropped
    EXPECT_EQ(out->asks[0].qty.raw, 554'122); // -0.00554122, likewise
    EXPECT_EQ(out->venue_seq, 0ULL);

    // 63950 restated at 0.0002
    EXPECT_EQ(book.apply(book_of(update, "ws_book_update.json")), BookResult::Applied);
    const auto after = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(after.has_value());
    expect_coherent(*after);
    EXPECT_EQ(after->bid_count, 25U) << "a restated level replaces, it does not add";
    EXPECT_EQ(after->bids[1].price.raw, 6'395'000'000'000);
    EXPECT_EQ(after->bids[1].qty.raw, 20'000);
}

TEST(BitfinexBook, ACountOfZeroDeletesOnTheSideItsSignNames) {
    const auto table = table_with_btc();
    auto book = book_for(table, "tBTCUSD");
    BookFrame snapshot_frame;
    BookFrame bid_delete;
    BookFrame ask_delete;
    EXPECT_EQ(book.apply(book_of(snapshot_frame, "ws_book_snapshot.json")), BookResult::Resynced);

    // [63951,0,1]: the amount only names the side
    EXPECT_EQ(book.apply(book_of(bid_delete, "ws_book_delete.json")), BookResult::Applied);
    const auto lifted = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(lifted.has_value());
    expect_coherent(*lifted);
    EXPECT_EQ(lifted->bid_count, 24U);
    EXPECT_EQ(lifted->bids[0].price.raw, 6'395'000'000'000) << "63950 stepped up to the touch";
    EXPECT_EQ(lifted->bids[0].qty.raw, 7'838'584);

    // [63963,0,-1] on the other side, from the same capture.
    EXPECT_EQ(book.apply(book_of(ask_delete, "ws_book_delete_ask.json")), BookResult::Applied);
    const auto both = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(both.has_value());
    expect_coherent(*both);
    EXPECT_EQ(both->ask_count, 24U);
    const auto gone =
        std::find_if(both->asks.begin(),
                     both->asks.begin() + both->ask_count,
                     [](const BookLevel& level) { return level.price.raw == 6'396'300'000'000; });
    EXPECT_EQ(gone, both->asks.begin() + both->ask_count);
}

TEST(BitfinexBook, TheHundredLevelSubscriptionFillsWhatSharedMemoryCarries) {
    const auto table = table_with_btc();
    auto book = book_for(table, "tBTCF0:USTF0");
    BookFrame snapshot_frame;
    const auto wire = book_of(snapshot_frame, "ws_book_snapshot_len100.json");
    EXPECT_EQ(wire.levels.size(), 200U);
    EXPECT_EQ(book.apply(wire), BookResult::Resynced);

    const auto out = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(out.has_value());
    expect_coherent(*out);
    EXPECT_EQ(out->bid_count, k_book_levels);
    EXPECT_EQ(out->ask_count, k_book_levels);
    EXPECT_EQ(out->bids[0].price.raw, 6'418'000'000'000);  // 64180
    EXPECT_EQ(out->asks[0].price.raw, 6'418'600'000'000);  // 64186
    EXPECT_EQ(out->bids[99].price.raw, 6'327'300'000'000); // the hundredth bid
    EXPECT_EQ(out->asks[99].price.raw, 6'558'300'000'000);
}

TEST(BitfinexBook, TheBookIsTheFastTopOfBookTheTickerIsNot) {
    const auto table = table_with_btc();
    auto book = book_for(table, "tBTCUSD");
    BookFrame snapshot_frame;
    EXPECT_EQ(book.apply(book_of(snapshot_frame, "ws_book_snapshot.json")), BookResult::Resynced);

    const auto out = book.snapshot(k_ingest, UnixNanos{9});
    const auto top = book.touch(k_ingest, UnixNanos{9});
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(top.has_value());
    EXPECT_EQ(top->instrument.packed, out->instrument.packed);
    EXPECT_EQ(top->bid.raw, out->bids[0].price.raw);
    EXPECT_EQ(top->ask.raw, out->asks[0].price.raw);
    EXPECT_EQ(top->bid_qty.raw, out->bids[0].qty.raw);
    EXPECT_EQ(top->ask_qty.raw, out->asks[0].qty.raw);
    EXPECT_LT(top->bid.raw, top->ask.raw);
    EXPECT_EQ(top->ts_init.ns, 9ULL);
}

TEST(BitfinexBook, ALevelOffTheGridInvalidatesTheWholeBook) {
    const auto table = table_with_btc();
    auto book = book_for(table, "tBTCUSD");
    BookFrame snapshot_frame;
    EXPECT_EQ(book.apply(book_of(snapshot_frame, "ws_book_snapshot.json")), BookResult::Resynced);

    // a ninth decimal, off the 1e-8 grid
    const std::array<WireLevel, 1> off_grid{
        WireLevel{.price = "63950.000000005", .amount = "0.0002", .count = 1}};
    WireBook bad{};
    bad.levels = off_grid;
    EXPECT_EQ(book.apply(bad), BookResult::BadLevel);
    EXPECT_FALSE(book.synced());
    EXPECT_FALSE(book.snapshot(k_ingest, k_ingest).has_value());

    // a positive count with zero amount is not a level
    auto again = book_for(table, "tBTCUSD");
    BookFrame again_snapshot;
    EXPECT_EQ(again.apply(book_of(again_snapshot, "ws_book_snapshot.json")), BookResult::Resynced);
    const std::array<WireLevel, 1> empty{WireLevel{.price = "63950", .amount = "0", .count = 1}};
    WireBook hollow{};
    hollow.levels = empty;
    EXPECT_EQ(again.apply(hollow), BookResult::BadLevel);
    EXPECT_FALSE(again.snapshot(k_ingest, k_ingest).has_value());
}

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

TEST(BitfinexWire, ShortPayloadIsAnErrorNotAPartialRead) {
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
    // tail growth must not break a reader
    const auto plain = parse_ticker(fixture("ws_ticker.json"));
    const auto stamped = parse_ticker(fixture("ws_ticker_timestamped.json"));
    ASSERT_TRUE(plain.has_value());
    ASSERT_TRUE(stamped.has_value());
    EXPECT_EQ(stamped->bid, "63914");
    EXPECT_EQ(stamped->ask, "63918");
}

TEST(BitfinexWire, EnvelopeTimestampIsReadWithoutParsingThePayload) {
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

TEST(BitfinexWire, PairInfoIsNestedThreeDeep) {
    static PairBuffer storage;
    const auto rows = parse_pair_info(fixture("rest_conf_pair.json"), storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);
    EXPECT_EQ((*rows)[0].pair, "BTCUSD"); // no `t` prefix on this endpoint
    EXPECT_EQ((*rows)[0].min_order_size, "0.00004");
    EXPECT_EQ((*rows)[0].max_order_size, "2000.0");
    // margin slots are null on this row
    EXPECT_EQ((*rows)[2].pair, "AAVE:USD");
    EXPECT_EQ((*rows)[2].min_order_size, "0.02");
}

TEST(BitfinexWire, PairInfoArityDiffersBetweenMarkets) {
    // 12 slots on spot, 10 on futures
    static PairBuffer storage;
    const auto rows = parse_pair_info(fixture("rest_conf_pair_futures.json"), storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);
    EXPECT_EQ((*rows)[0].pair, "BTCF0:USTF0");
    EXPECT_EQ((*rows)[0].min_order_size, "0.00004");
    EXPECT_EQ((*rows)[2].pair, "AUSTRALIA200IXF0:USTF0");
}

TEST(BitfinexMap, PaperTradingPairsAreRefused) {
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
    EXPECT_FALSE(
        map_instrument({.pair = "BTCF0:USTF0", .min_order_size = "0.00004"}, MarketType::Spot)
            .has_value());
    EXPECT_FALSE(map_instrument({.pair = "BTCUSD", .min_order_size = "0.00004"}, MarketType::Perp)
                     .has_value());
    EXPECT_FALSE(map_instrument({.pair = "BTCUSD", .min_order_size = "0.00004"}, MarketType::Future)
                     .has_value());
}

TEST(BitfinexMap, EveryInstrumentCarriesTheSameDerivedGrid) {
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

TEST(BitfinexMap, ExponentAwareParserIsExactOrRefuses) {
    EXPECT_EQ(parse_number_e9("63947")->raw, 63'947'000'000'000);
    EXPECT_EQ(parse_number_e9("2.1873936")->raw, 2'187'393'600);
    EXPECT_EQ(parse_number_e9("1e-8")->raw, 10);
    EXPECT_EQ(parse_number_e9("-0.0002")->raw, -200'000);
    EXPECT_EQ(parse_number_e9("8.5e-05")->raw, 85'000);
    EXPECT_EQ(parse_number_e9("1E+3")->raw, 1'000'000'000'000);
    EXPECT_FALSE(parse_number_e9("1e-10").has_value());
    EXPECT_FALSE(parse_number_e9("0.0000000001").has_value());
    EXPECT_FALSE(parse_number_e9("").has_value());
    EXPECT_FALSE(parse_number_e9("1e").has_value());
    EXPECT_FALSE(parse_number_e9("--1").has_value());
    EXPECT_FALSE(parse_number_e9("1.2.3").has_value());
}

TEST(BitfinexMap, PriceFinerThanTheGridIsDroppedNotRounded) {
    const auto table = table_with_btc();
    WireTicker wire{};
    wire.bid = "63947.000000001";
    wire.bid_size = "1";
    wire.ask = "63948";
    wire.ask_size = "1";
    EXPECT_FALSE(map_bbo(wire, "tBTCUSD", table, UnixNanos{0}, k_ingest).has_value());
}

TEST(BitfinexMap, EveryPriceTheVenueActuallySendsDividesExactly) {
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
        R"({"event":"subscribe","channel":"book","symbol":"tBTCUSD","prec":"P0","freq":"F0","len":"100"})");

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
    const auto greeting = parse_frame(fixture("ws_info.json"));
    ASSERT_TRUE(greeting.has_value());
    EXPECT_EQ(greeting->kind, FrameKind::Info);
    EXPECT_EQ(greeting->code, 0);

    const auto notice = parse_frame(R"({"event":"info","code":20061,"msg":"maintenance ended"})");
    ASSERT_TRUE(notice.has_value());
    EXPECT_EQ(notice->kind, FrameKind::Info);
    EXPECT_EQ(notice->code, k_info_maintenance_end);
}

TEST(BitfinexRest, PublicReadsTargetTheirEndpoints) {
    RequestBuilder builder;
    EXPECT_EQ(rest_host(), "api-pub.bitfinex.com");
    EXPECT_EQ(builder.platform_status().target, "/v2/platform/status");
    EXPECT_EQ(builder.pair_info(false).target, "/v2/conf/pub:info:pair");
    EXPECT_EQ(builder.pair_info(true).target, "/v2/conf/pub:info:pair:futures");
    EXPECT_EQ(builder.book("tBTCUSD", 25).target, "/v2/book/tBTCUSD/P0?len=25");
    EXPECT_EQ(builder.tickers().target, "/v2/tickers?symbols=ALL");
}

namespace {

// made-up credentials; digests are rfc 4231's since bitfinex publishes no vector
constexpr std::string_view k_api_key = "bfx-test-key";
constexpr std::string_view k_api_secret = "bfx-test-secret";

// nonce 1700000000000000
constexpr UnixNanos k_sign_now{1'700'000'000'000'000'000ULL};
constexpr std::string_view k_nonce = "1700000000000000";

std::string_view header_of(const net::RestRequest& request, std::string_view name) {
    for (const auto& header : request.headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return {};
}

std::string digest_of(std::string_view message) {
    std::array<char, k_hex384_bytes> out{};
    return std::string{hmac_sha384_hex(k_api_secret, message, out)};
}

} // namespace

// rfc 4231 test case 1
TEST(BitfinexSign, HmacSha384MatchesTheStandardVector) {
    const std::string key(20, '\x0b');
    std::array<char, k_hex384_bytes> out{};
    EXPECT_EQ(hmac_sha384_hex(key, "Hi There", out),
              "afd03944d84895626b0825f4ab46907f15f9dadbe4101ec682aa034c7cebc59c"
              "faea9ea9076ede7f4af152e8b2fa9cb6");

    std::array<char, k_hex384_bytes - 1> small{};
    EXPECT_TRUE(hmac_sha384_hex(key, "Hi There", small).empty());
}

TEST(BitfinexSign, Sha384IsNotTruncatedSha512) {
    const std::string key(20, '\x0b');
    std::array<char, k_hex384_bytes> sha384{};
    std::array<char, net::k_hex512_bytes> sha512{};
    const auto short_digest = hmac_sha384_hex(key, "Hi There", sha384);
    const auto long_digest = net::hmac_sha512_hex(key, "Hi There", sha512);
    ASSERT_EQ(short_digest.size(), k_hex384_bytes);
    ASSERT_EQ(long_digest.size(), net::k_hex512_bytes);
    EXPECT_NE(short_digest, long_digest.substr(0, k_hex384_bytes));
}

TEST(BitfinexSign, NoncesStrictlyIncreaseEvenWithinOneMicrosecond) {
    NonceSource nonces;
    const auto first = nonces.next(k_sign_now.ns);
    EXPECT_EQ(first, 1'700'000'000'000'000ULL);
    const auto second = nonces.next(k_sign_now.ns);
    EXPECT_EQ(second, first + 1);
    EXPECT_GT(nonces.next(k_sign_now.ns - 1'000'000'000ULL), second);
}

TEST(BitfinexSign, TheWebsocketAuthPayloadRepeatsTheNonce) {
    std::array<char, 512> buffer{};
    const auto frame = ws_auth_frame(k_api_key, k_api_secret, 1'700'000'000'000'000ULL, buffer);
    ASSERT_FALSE(frame.empty());

    const std::string payload = std::string{"AUTH"} + std::string{k_nonce} + std::string{k_nonce};
    const auto expected = digest_of(payload);
    EXPECT_EQ(std::string{frame},
              std::string{R"({"event":"auth","apiKey":"bfx-test-key","authSig":")"} + expected +
                  R"(","authPayload":"AUTH17000000000000001700000000000000",)"
                  R"("authNonce":1700000000000000})");

    EXPECT_TRUE(ws_auth_frame({}, k_api_secret, 1, buffer).empty());
    EXPECT_TRUE(ws_auth_frame(k_api_key, {}, 1, buffer).empty());
}

TEST(BitfinexSign, TheRestDigestCoversAPrefixThatIsNotSent) {
    config::Secret secret;
    ASSERT_TRUE(secret.assign(k_api_secret).has_value());
    SignedRequestBuilder builder{k_api_key, secret};

    const auto request = builder.open_orders(MarketType::Perp, k_sign_now);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());
    EXPECT_EQ(request->method, net::HttpMethod::Post);
    EXPECT_EQ(request->target, "/v2/auth/r/orders");
    EXPECT_EQ(request->body, "{}");
    EXPECT_EQ(header_of(*request, "bfx-nonce"), k_nonce);
    EXPECT_EQ(header_of(*request, "bfx-apikey"), k_api_key);

    const std::string message =
        std::string{"/api/v2/auth/r/orders"} + std::string{k_nonce} + std::string{"{}"};
    EXPECT_EQ(builder.last_sign_message(), message);
    EXPECT_EQ(header_of(*request, "bfx-signature"), digest_of(message));
}

TEST(BitfinexSign, WithoutCredentialsNothingIsBuilt) {
    const config::Secret empty;
    SignedRequestBuilder unsigned_builder{k_api_key, empty};
    const auto request = unsigned_builder.open_orders(MarketType::Perp, k_sign_now);
    ASSERT_FALSE(request.has_value());
    EXPECT_EQ(request.error(), BuildError::NotSigned);
}

TEST(BitfinexClientId, TheCidIsDerivedFromTheUlidAndFitsTheField) {
    const auto ulid = *ExchangeClientOrderId::from("01ARZ3NDEKTSV4RRFFQ69G5FAV");
    const auto cid = cid_from(ulid);
    ASSERT_TRUE(cid.has_value());
    EXPECT_GE(*cid, 0);
    EXPECT_LT(*cid, std::int64_t{1} << k_cid_bits);
    EXPECT_EQ(cid_from(ulid), cid);

    UlidGen gen{11};
    std::vector<std::int64_t> seen;
    for (int i = 0; i < 2048; ++i) {
        const auto id = gen.next(UnixNanos{1'700'000'000'000'000'000ULL + std::uint64_t(i)});
        const auto derived = cid_from(id);
        ASSERT_TRUE(derived.has_value());
        seen.push_back(*derived);
    }
    std::ranges::sort(seen);
    EXPECT_EQ(std::ranges::unique(seen).begin(), seen.end());

    EXPECT_FALSE(cid_from(*ExchangeClientOrderId::from("short")).has_value());
    EXPECT_FALSE(cid_from(*ExchangeClientOrderId::from("01ARZ3NDEKTSV4RRFFQ69G5FAU")).has_value());
}

namespace {

OrderCmd order_for(const exec::InstrumentTable& table, std::string_view symbol, Side side) {
    OrderCmd cmd{};
    cmd.instrument = base_of(*table.find(symbol)).id;
    cmd.side = side;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    // exact on the 1e-8 grid: 63947.0 and 0.1
    cmd.price = PriceTicks::from_raw(6'394'700'000'000);
    cmd.qty = QtyLots::from_raw(10'000'000);
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("01ARZ3NDEKTSV4RRFFQ69G5FAV");
    return cmd;
}

} // namespace

TEST(BitfinexRest, ASellIsANegativeAmountAndNothingElse) {
    const auto table = table_with_btc();
    config::Secret secret;
    ASSERT_TRUE(secret.assign(k_api_secret).has_value());
    SignedRequestBuilder builder{k_api_key, secret};

    const auto buy =
        builder.create_order(order_for(table, "tBTCUSD", Side::Buy), table, k_sign_now);
    ASSERT_TRUE(buy.has_value()) << to_string(buy.error());
    EXPECT_EQ(buy->target, "/v2/auth/w/order/submit");
    EXPECT_NE(std::string{buy->body}.find(R"("amount":"0.1")"), std::string::npos) << buy->body;

    const auto sell =
        builder.create_order(order_for(table, "tBTCUSD", Side::Sell), table, k_sign_now);
    ASSERT_TRUE(sell.has_value());
    EXPECT_NE(std::string{sell->body}.find(R"("amount":"-0.1")"), std::string::npos) << sell->body;
}

TEST(BitfinexRest, SpotAndDerivativesTakeDifferentTypeNames) {
    EXPECT_EQ(order_type(MarketType::Spot, OrderType::Limit, TimeInForce::Gtc), "EXCHANGE LIMIT");
    EXPECT_EQ(order_type(MarketType::Perp, OrderType::Limit, TimeInForce::Gtc), "LIMIT");
    EXPECT_EQ(order_type(MarketType::Spot, OrderType::Market, TimeInForce::Gtc), "EXCHANGE MARKET");
    EXPECT_EQ(order_type(MarketType::Perp, OrderType::Market, TimeInForce::Gtc), "MARKET");
    EXPECT_EQ(order_type(MarketType::Spot, OrderType::Limit, TimeInForce::Ioc), "EXCHANGE IOC");
    EXPECT_EQ(order_type(MarketType::Perp, OrderType::Limit, TimeInForce::Fok), "FOK");

    EXPECT_EQ(order_flags(OrderFlags::None), 0);
    EXPECT_EQ(order_flags(OrderFlags::PostOnly), k_flag_post_only);
    EXPECT_EQ(order_flags(OrderFlags::ReduceOnly), k_flag_reduce_only);
}

TEST(BitfinexRest, TheWholeCreateBodyRidesTheSignature) {
    const auto table = table_with_btc();
    config::Secret secret;
    ASSERT_TRUE(secret.assign(k_api_secret).has_value());
    SignedRequestBuilder builder{k_api_key, secret};

    auto cmd = order_for(table, "tBTCF0:USTF0", Side::Sell);
    cmd.flags = OrderFlags::PostOnly;
    const auto request = builder.create_order(cmd, table, k_sign_now);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());
    const auto cid = *cid_from(cmd.exchange_client_order_id);
    EXPECT_EQ(std::string{request->body},
              std::string{R"({"type":"LIMIT","symbol":"tBTCF0:USTF0","amount":"-0.1",)"
                          R"("price":"63947","cid":)"} +
                  std::to_string(cid) + R"(,"flags":4096})");
    EXPECT_NE(std::string{builder.last_sign_message()}.find(std::string{request->body}),
              std::string::npos);
}

TEST(BitfinexRest, CancelAndAmendNeedTheVenueOrderId) {
    const auto table = table_with_btc();
    config::Secret secret;
    ASSERT_TRUE(secret.assign(k_api_secret).has_value());
    SignedRequestBuilder builder{k_api_key, secret};

    auto cmd = order_for(table, "tBTCUSD", Side::Sell);
    EXPECT_EQ(builder.cancel_order(cmd, table, k_sign_now).error(), BuildError::NoOrderTarget);
    EXPECT_EQ(builder.amend_order(cmd, table, k_sign_now).error(), BuildError::NoOrderTarget);

    cmd.target_exchange_order_id = *ExchangeOrderId::from("1234567891");
    const auto cancel = builder.cancel_order(cmd, table, k_sign_now);
    ASSERT_TRUE(cancel.has_value());
    EXPECT_EQ(cancel->target, "/v2/auth/w/order/cancel");
    EXPECT_EQ(cancel->body, R"({"id":1234567891})");

    const auto amend = builder.amend_order(cmd, table, k_sign_now);
    ASSERT_TRUE(amend.has_value());
    EXPECT_EQ(amend->target, "/v2/auth/w/order/update");
    EXPECT_EQ(amend->body, R"({"id":1234567891,"amount":"-0.1","price":"63947"})");
}

TEST(BitfinexWire, TheAuthAckSeparatesOkFromRefused) {
    const auto ok = parse_auth_ack(fixture("ws_auth_ok.json"));
    ASSERT_TRUE(ok.has_value());
    EXPECT_TRUE(ok->ok);

    const auto failed = parse_auth_ack(fixture("ws_auth_failed.json"));
    ASSERT_TRUE(failed.has_value());
    EXPECT_FALSE(failed->ok);
    EXPECT_EQ(failed->code, k_auth_failed);
    EXPECT_EQ(classify(failed->code), exec::ErrorClass::AuthFailure);

    const auto info = parse_frame(fixture("ws_auth_ok.json"));
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->kind, FrameKind::Auth);
}

TEST(BitfinexWire, OrderStatusIsMatchedByPrefix) {
    EXPECT_EQ(order_status("ACTIVE"), WireOrderStatus::Active);
    EXPECT_EQ(order_status("EXECUTED @ 63951.0(-0.1)"), WireOrderStatus::Executed);
    EXPECT_EQ(order_status("PARTIALLY FILLED @ 63951.0(-0.05)"), WireOrderStatus::PartiallyFilled);
    EXPECT_EQ(order_status("CANCELED"), WireOrderStatus::Canceled);
    EXPECT_EQ(order_status("POSTONLY CANCELED"), WireOrderStatus::Canceled);
    EXPECT_EQ(order_status("INSUFFICIENT MARGIN was: 0.0 : 100.0"), WireOrderStatus::Rejected);
    EXPECT_EQ(order_status("RSN_DUST"), WireOrderStatus::Rejected);
    EXPECT_EQ(order_status("SOMETHING BITFINEX ADDED"), WireOrderStatus::Unknown);
}

TEST(BitfinexWire, OrdersArriveOneAtATimeAndAsASnapshot) {
    const auto one = parse_order(fixture("ws_order_new.json"));
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(one->id, "1234567890");
    EXPECT_EQ(one->symbol, "tBTCUSD");
    EXPECT_EQ(one->amount, "0.1");
    EXPECT_EQ(one->amount_orig, "0.1");
    EXPECT_EQ(one->type, "EXCHANGE LIMIT");
    EXPECT_EQ(one->price, "63947");
    EXPECT_EQ(one->status, WireOrderStatus::Active);
    EXPECT_EQ(one->cid, 987654321);
    EXPECT_EQ(one->mts_update, 1700000000100U);

    static thread_local OrderBuffer storage;
    const auto snapshot = parse_orders(fixture("ws_orders_snapshot.json"), storage);
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->size(), 3U);
    EXPECT_EQ((*snapshot)[1].amount, "-0.05");
    EXPECT_EQ((*snapshot)[2].symbol, "tBTCF0:USTF0");
    EXPECT_EQ((*snapshot)[2].flags, k_flag_reduce_only);

    // the same rows arrive bare from rest
    const auto rest = parse_orders(fixture("rest_orders.json"), storage);
    ASSERT_TRUE(rest.has_value());
    EXPECT_EQ(rest->size(), 3U);
    EXPECT_EQ((*rest)[0].id, "1234567890");
}

TEST(BitfinexWire, ExecutionsStateTheirFeeOnlyOnTheSecondFrame) {
    const auto te = parse_execution(fixture("ws_exec_te.json"));
    ASSERT_TRUE(te.has_value());
    EXPECT_EQ(te->id, "1111");
    EXPECT_EQ(te->order_id, "1234567891");
    EXPECT_EQ(te->amount, "-0.05");
    EXPECT_TRUE(te->fee.empty()); // `te` arrives before the fee is known
    EXPECT_TRUE(te->liquidity_known);
    EXPECT_TRUE(te->maker);

    const auto tu = parse_execution(fixture("ws_exec_tu.json"));
    ASSERT_TRUE(tu.has_value());
    EXPECT_EQ(tu->fee, "-0.0639");
    EXPECT_EQ(tu->fee_currency, "USD");

    static thread_local ExecutionBuffer storage;
    const auto rest = parse_executions(fixture("rest_trades.json"), storage);
    ASSERT_TRUE(rest.has_value());
    ASSERT_EQ(rest->size(), 2U);
    EXPECT_TRUE((*rest)[0].maker);
    EXPECT_TRUE((*rest)[1].liquidity_known);
    EXPECT_FALSE((*rest)[1].maker);
}

TEST(BitfinexWire, PositionsAndNotificationsCarryTheirVerdicts) {
    static thread_local PositionBuffer storage;
    const auto positions = parse_positions(fixture("ws_positions_snapshot.json"), storage);
    ASSERT_TRUE(positions.has_value());
    ASSERT_EQ(positions->size(), 2U);
    EXPECT_EQ((*positions)[0].amount, "-0.5");
    EXPECT_EQ((*positions)[0].status, "ACTIVE");
    EXPECT_EQ((*positions)[1].status, "CLOSED");

    const auto ok = parse_notification(fixture("rest_order_submit.json"));
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->type, "on-req");
    EXPECT_TRUE(notification_ok(*ok));
    ASSERT_TRUE(ok->has_order);
    EXPECT_EQ(ok->order.id, "1234567890");

    const auto refused = parse_notification(fixture("rest_order_error.json"));
    ASSERT_TRUE(refused.has_value());
    EXPECT_FALSE(notification_ok(*refused));
    EXPECT_EQ(refused->code, k_unknown_pair);
    EXPECT_FALSE(refused->text.empty());

    // the socket wraps the same array as [0,"n",[...]]
    const auto pushed = parse_notification(fixture("ws_notification_error.json"));
    ASSERT_TRUE(pushed.has_value());
    EXPECT_EQ(pushed->type, "on-req");
    EXPECT_FALSE(notification_ok(*pushed));
}

TEST(BitfinexMap, TheSideComesFromTheOriginalAmountNotTheRemainder) {
    const auto table = table_with_btc();
    const auto executed = parse_order(fixture("ws_order_executed.json"));
    ASSERT_TRUE(executed.has_value());
    EXPECT_EQ(executed->amount, "0"); // nothing left, and no sign left either
    EXPECT_EQ(executed->amount_orig, "-0.1");

    const auto report = map_open_order(*executed, table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->side, Side::Sell);
    EXPECT_EQ(report->status, OrderStatus::Filled);
    EXPECT_EQ(report->qty.raw, 10'000'000);
    EXPECT_EQ(report->leaves.raw, 0);

    EXPECT_FALSE(map_order_event(*executed, table, ExchangeClientOrderId{}, k_ingest).has_value());
}

TEST(BitfinexMap, AnActiveOrderBecomesAcceptedAndACancelClosesIt) {
    const auto table = table_with_btc();
    const auto id = *ExchangeClientOrderId::from("01ARZ3NDEKTSV4RRFFQ69G5FAV");

    const auto fresh = parse_order(fixture("ws_order_new.json"));
    ASSERT_TRUE(fresh.has_value());
    const auto accepted = map_order_event(*fresh, table, id, k_ingest);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(accepted->type, OrderEventType::Accepted);
    EXPECT_EQ(accepted->side, Side::Buy);
    EXPECT_EQ(accepted->qty.raw, 10'000'000);
    EXPECT_EQ(accepted->leaves_qty.raw, 10'000'000);
    EXPECT_EQ(accepted->price.raw, 6'394'700'000'000);
    EXPECT_EQ(accepted->exchange_order_id.view(), "1234567890");
    EXPECT_EQ(accepted->exchange_client_order_id.view(), id.view());

    const auto cancelled = parse_order(fixture("ws_order_canceled.json"));
    ASSERT_TRUE(cancelled.has_value());
    const auto event = map_order_event(*cancelled, table, id, k_ingest);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::Canceled);
    EXPECT_EQ(event->side, Side::Sell);
    EXPECT_EQ(event->leaves_qty.raw, 0);

    const auto postonly = parse_order(fixture("ws_order_postonly_canceled.json"));
    ASSERT_TRUE(postonly.has_value());
    const auto refused = map_order_event(*postonly, table, id, k_ingest);
    ASSERT_TRUE(refused.has_value());
    EXPECT_EQ(refused->type, OrderEventType::Canceled);
    EXPECT_EQ(refused->flags, OrderFlags::PostOnly);
}

TEST(BitfinexMap, AFillTakesItsSideAndItsCostFromTheExecution) {
    const auto table = table_with_btc();
    const auto tu = parse_execution(fixture("ws_exec_tu.json"));
    ASSERT_TRUE(tu.has_value());

    const OrderContext context{.client_order_id =
                                   *ExchangeClientOrderId::from("01ARZ3NDEKTSV4RRFFQ69G5FAV"),
                               .leaves = QtyLots::from_raw(5'000'000)};
    const auto fill = map_execution(*tu, table, context, k_ingest);
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->type, OrderEventType::Filled);
    EXPECT_EQ(fill->side, Side::Sell);
    EXPECT_EQ(fill->qty.raw, 5'000'000); // 0.05 on a 1e-8 grid
    EXPECT_EQ(fill->leaves_qty.raw, 5'000'000);
    EXPECT_EQ(fill->liquidity, Liquidity::Maker);
    EXPECT_EQ(fill->exec_id.view(), "1111");
    EXPECT_EQ(fill->exchange_order_id.view(), "1234567891");
    EXPECT_EQ(fill->exchange_client_order_id.view(), "01ARZ3NDEKTSV4RRFFQ69G5FAV");
    // a charge arrives negative and comes out positive
    EXPECT_EQ(fill->fee.raw, 63'900'000);

    const auto te = parse_execution(fixture("ws_exec_te.json"));
    ASSERT_TRUE(te.has_value());
    const auto early = map_execution(*te, table, context, k_ingest);
    ASSERT_TRUE(early.has_value());
    EXPECT_EQ(early->fee.raw, 0);
}

TEST(BitfinexMap, APositionsSignIsTheOnlyThingThatSaysWhichWay) {
    const auto table = table_with_btc();
    static thread_local PositionBuffer storage;
    const auto positions = parse_positions(fixture("rest_positions.json"), storage);
    ASSERT_TRUE(positions.has_value());
    ASSERT_EQ(positions->size(), 2U);

    const auto shorted = map_position((*positions)[0], table);
    ASSERT_TRUE(shorted.has_value());
    EXPECT_EQ(shorted->net_qty.raw, -50'000'000); // -0.5 on a 1e-8 grid
    EXPECT_EQ(shorted->avg_entry.raw, 6'394'700'000'000);

    EXPECT_FALSE(map_position((*positions)[1], table).has_value());

    static thread_local PositionBuffer snapshot_storage;
    const auto snapshot = parse_positions(fixture("ws_positions_snapshot.json"), snapshot_storage);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_FALSE(map_position((*snapshot)[1], table).has_value());
}

TEST(BitfinexErrors, TheLiveAuthRefusalClassifiesAsACredentialProblem) {
    // http 500 for a bad key; the body decides
    const auto& body = fixture("rest_auth_error.json");
    ASSERT_TRUE(is_rest_error(body));
    const auto err = parse_rest_error(body);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, k_auth_failed);
    EXPECT_EQ(classify(err->code), exec::ErrorClass::AuthFailure);
    // a stale nonce is retryable
    EXPECT_EQ(classify(k_auth_nonce), exec::ErrorClass::Retryable);
}

} // namespace
} // namespace raikiri::venues::bitfinex
