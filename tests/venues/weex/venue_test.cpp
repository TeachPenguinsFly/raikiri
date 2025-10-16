#include <raikiri/venues/weex/book.hpp>
#include <raikiri/venues/weex/catalogue.hpp>
#include <raikiri/venues/weex/endpoints.hpp>
#include <raikiri/venues/weex/envelope.hpp>
#include <raikiri/venues/weex/errors.hpp>
#include <raikiri/venues/weex/map.hpp>
#include <raikiri/venues/weex/market_data.hpp>
#include <raikiri/venues/weex/rest.hpp>
#include <raikiri/venues/weex/weex_venue.hpp>
#include <raikiri/venues/weex/ws.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::venues::weex {
namespace {

// There is no RAIKIRI_WEEX_FIXTURES define: this target is the venue test, so
// the fixture directory is located from this file rather than from CMake.
//
// HELD, not returned by value. Every wire type in this adapter is a set of
// VIEWS over the bytes it was parsed from, so a fixture read into a temporary
// leaves every field of every parse pointing at freed memory — which reads as
// a passing test until a sanitiser is asked.
const std::string& fixture(std::string_view name) {
    static std::map<std::string, std::string, std::less<>> held;
    if (const auto found = held.find(name); found != held.end()) {
        return found->second;
    }
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    auto text = buf.str();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return held.emplace(std::string{name}, std::move(text)).first->second;
}

// A one-field edit of real bytes, used only where the venue has never sent the
// shape a rule exists to handle. Each caller says which field it removed and
// why no capture carries it.
std::string without(std::string_view body, std::string_view field) {
    std::string out{body};
    const auto at = out.find(field);
    EXPECT_NE(at, std::string::npos) << field;
    out.erase(at, field.size());
    return out;
}

constexpr UnixNanos k_ingest{1'788'774'086'500'000'000ULL};

class CaptureSink final : public exec::GatewaySink {
public:
    void on_bbo(const Bbo&) noexcept override {}

    void on_trade(const TradeTick&) noexcept override {}

    void on_funding(const FundingUpdate&) noexcept override {}

    void on_order_event(const OrderEvent& event) noexcept override { events.push_back(event); }

    void on_connectivity(const exec::ConnEvent&) noexcept override {}

    std::vector<OrderEvent> events;
};

// The catalogue fixture, interned. Every mapper test that needs a real tick or
// a real lot goes through this rather than inventing one.
exec::InstrumentTable load_table() {
    static thread_local InstrumentBuffer storage;
    const auto body = fixture("rest_exchange_info.json");
    const auto rows = parse_exchange_info(body, storage);
    EXPECT_TRUE(rows.has_value());
    exec::InstrumentTable table;
    for (const auto& row : *rows) {
        if (auto instrument = map_instrument(row, MarketType::Perp); instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value()) << row.symbol;
        }
    }
    return table;
}

[[nodiscard]] const InstrumentBase& listed(const exec::InstrumentTable& table,
                                           std::string_view symbol) {
    const auto* instrument = table.find(symbol);
    EXPECT_NE(instrument, nullptr) << symbol;
    return base_of(*instrument);
}

// ------------------------------------------------------------- venue ----

TEST(WeexVenue, RegistersAndBuilds) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto adapter =
        exec::make_venue({.venue = VenueId::Weex, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Weex);
    EXPECT_NE(adapter->data, nullptr);
    EXPECT_NE(adapter->execution, nullptr);
    EXPECT_NE(adapter->instruments, nullptr);
}

TEST(WeexVenue, TheCapsRowClaimsMarketDataAndNothingElse) {
    // The row is what a caller consults BEFORE acting. Claiming an order path
    // this adapter does not have would be discovered one command at a time.
    const auto& caps = exec::caps_of(VenueId::Weex);
    EXPECT_TRUE(caps.markets.perp);
    EXPECT_FALSE(caps.markets.spot);
    EXPECT_FALSE(caps.markets.future);
    EXPECT_FALSE(caps.markets.option);
    EXPECT_FALSE(caps.ws_order_entry);
    EXPECT_FALSE(caps.fix_order_entry);
    // Not on the market it serves, and not on any other: this adapter has no
    // private REST at all.
    EXPECT_FALSE(caps.position_pull.any());
    EXPECT_FALSE(caps.order_pull.any());
    EXPECT_FALSE(caps.fill_pull.any());
    EXPECT_FALSE(caps.balance_pull.any());
    EXPECT_FALSE(caps.funding_charge.any());
    EXPECT_EQ(caps.user_stream, exec::UserStreamModel::None);
    // The one public capability that IS claimed, and the ticker channel is
    // what delivers it.
    EXPECT_TRUE(caps.ticker_stats);
}

TEST(WeexVenue, EveryCommandIsRefusedLocallyRatherThanLeftOpen) {
    // Silence is the one answer an order may never get: a command that vanishes
    // sits open in the store forever, indistinguishable from one the venue is
    // still thinking about.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter =
        exec::make_venue({.venue = VenueId::Weex, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.instrument = InstrumentId::make(VenueId::Weex, MarketType::Perp, SymbolId{0});
    cmd.client_order_id = ClientOrderId::make(1, 42);
    cmd.qty = QtyLots::from_raw(7);
    adapter->execution->submit(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);
    // A create that never reached the book leaves nothing behind.
    EXPECT_EQ(sink.events[0].leaves_qty.raw, 0);

    cmd.kind = CmdKind::Cancel;
    adapter->execution->cancel(cmd);
    ASSERT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(sink.events[1].type, OrderEventType::CancelRejected);
    // A refused cancel leaves the order exactly as it was.
    EXPECT_EQ(sink.events[1].leaves_qty.raw, 7);

    adapter->execution->amend(cmd);
    ASSERT_EQ(sink.events.size(), 3U);
    EXPECT_EQ(sink.events[2].type, OrderEventType::ModifyRejected);

    // And the pulls state their absence rather than answering "nothing".
    EXPECT_FALSE(adapter->execution->open_order_reports(MarketType::Perp).has_value());
    EXPECT_FALSE(adapter->execution->position_reports(MarketType::Perp).has_value());
    EXPECT_FALSE(adapter->execution->fill_reports(MarketType::Perp, UnixNanos{}).has_value());
    EXPECT_FALSE(adapter->execution->balance_reports(MarketType::Perp).has_value());
}

TEST(WeexVenue, MarketsWithNoCatalogueLoaderAreRefusedRatherThanReturnedEmpty) {
    // An empty catalogue reads as "this venue lists nothing", which is a very
    // different claim from "this adapter does not load that market". Every
    // refusal happens before a socket opens, so this test touches no network.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter =
        exec::make_venue({.venue = VenueId::Weex, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    for (const auto market : {MarketType::Spot, MarketType::Future, MarketType::Option}) {
        const auto loaded = adapter->instruments->fetch_all(market);
        ASSERT_FALSE(loaded.has_value()) << to_string(market);
        EXPECT_EQ(loaded.error().cls, exec::ErrorClass::Terminal);
    }
}

// ---------------------------------------------------------- envelope ----

TEST(WeexWire, EventDispatchResolvesKindAndSymbolFromTheFrame) {
    for (const auto& [file, kind] : {std::pair{"ws_depth_snapshot.json", EventKind::DepthSnapshot},
                                     std::pair{"ws_depth_update.json", EventKind::Depth},
                                     std::pair{"ws_trade_buy.json", EventKind::Trade},
                                     std::pair{"ws_trade_snapshot.json", EventKind::TradeSnapshot},
                                     std::pair{"ws_ticker.json", EventKind::Ticker}}) {
        const auto frame = fixture(file);
        const auto event = parse_event(frame);
        ASSERT_TRUE(event.has_value()) << file;
        EXPECT_EQ(event->kind, kind) << file;
        // The symbol is read here because a trade frame names it once and
        // carries several prints under it.
        EXPECT_FALSE(event->symbol.empty()) << file;
    }
}

TEST(WeexWire, TheSnapshotIsADIFFERENTKINDFromTheDeltaOnTheSameChannel) {
    // Both arrive on <sym>@depth15 and only "e" separates them. A snapshot
    // dispatched as a delta would be chained onto a book it replaces.
    const auto snapshot = parse_event(fixture("ws_depth_snapshot.json"));
    const auto delta = parse_event(fixture("ws_depth_update.json"));
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_TRUE(delta.has_value());
    EXPECT_NE(snapshot->kind, delta->kind);
    // The trade channel splits the same way, and its backfill is dropped for
    // reasons the client states rather than published as prints.
    EXPECT_NE(parse_event(fixture("ws_trade_snapshot.json"))->kind,
              parse_event(fixture("ws_trade_buy.json"))->kind);
}

TEST(WeexWire, AnAcknowledgementAndARefusalAreOneBooleanApart) {
    // WEEX answers both with {"result":...,"id":n}. Reading a refusal as an
    // acknowledgement is indistinguishable from a channel with nothing to say.
    const auto ack = parse_event(fixture("ws_subscribe_ack.json"));
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->kind, EventKind::SubscribeAck);

    const auto refused = fixture("ws_subscribe_refused.json");
    const auto bad = parse_event(refused);
    ASSERT_TRUE(bad.has_value());
    EXPECT_EQ(bad->kind, EventKind::Error);

    // And the reason is prose, not a code: WEEX states no numeric code on the
    // socket, so nothing here pretends to one.
    const auto err = parse_stream_error(refused);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->id, 1U);
    EXPECT_EQ(err->msg, "INVALID_ARGUMENT: invalid event : bookticker");
}

TEST(WeexWire, ControlFramesAreNotMistakenForData) {
    EXPECT_EQ(parse_event(fixture("ws_ping.json"))->kind, EventKind::Ping);
    EXPECT_EQ(parse_event(fixture("ws_connected.json"))->kind, EventKind::Connected);
}

TEST(WeexWire, ARestFailureIsDetectedBySignAndNotByKey) {
    for (const auto& [file, code] : {std::pair{"rest_error_param_empty.json", k_param_empty},
                                     std::pair{"rest_error_invalid_param.json", k_param_invalid}}) {
        const auto err = parse_rest_error(fixture(file));
        ASSERT_TRUE(err.has_value()) << file;
        EXPECT_EQ(err->code, code) << file;
        EXPECT_FALSE(err->msg.empty()) << file;
        EXPECT_EQ(classify(err->code), exec::ErrorClass::Terminal) << file;
    }
    // A path WEEX does not route answers a shape with a POSITIVE `status` and
    // no `code` at all. It reads here as a success payload, and the caller's
    // own HTTP check is what catches it — which is the honest split, because
    // this reader's whole job is the negative-code convention.
    const auto missing = parse_rest_error(fixture("rest_error_not_found.json"));
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(missing->code, k_ok);
    // A real success body carries no code either.
    EXPECT_EQ(parse_rest_error(fixture("rest_exchange_info.json"))->code, k_ok);
    // The bulk reads answer with a bare ARRAY, which is not an object at all.
    EXPECT_EQ(parse_rest_error(fixture("rest_premium_index.json"))->code, k_ok);
}

// -------------------------------------------------------------- book ----

TEST(WeexBook, TheEventAndTheDispositionMustAgree) {
    static thread_local LevelBuffer storage;
    const auto snapshot = parse_depth(fixture("ws_depth_snapshot.json"), storage);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_TRUE(snapshot->is_snapshot);
    EXPECT_EQ(snapshot->tier, 15U);

    const auto delta = parse_depth(fixture("ws_depth_update.json"), storage);
    ASSERT_TRUE(delta.has_value());
    EXPECT_FALSE(delta->is_snapshot);

    // EDITED, one field: no live frame disagrees with itself, which is exactly
    // why the rule needs a test. A snapshot applied as a delta leaves a book
    // that is a mixture of two moments and still reports itself live.
    auto crossed = fixture("ws_depth_snapshot.json");
    crossed.replace(crossed.find(R"("e":"depthSnapshot")"),
                    std::string_view{R"("e":"depthSnapshot")"}.size(),
                    R"("e":"depth")");
    EXPECT_FALSE(parse_depth(crossed, storage).has_value());
}

TEST(WeexBook, TheChainSharesItsBoundaryRatherThanSucceedingIt) {
    static thread_local LevelBuffer storage;
    const auto snapshot = parse_depth(fixture("ws_depth_snapshot.json"), storage);
    ASSERT_TRUE(snapshot.has_value());
    const auto first_u = snapshot->last_id;

    const auto delta = parse_depth(fixture("ws_depth_update.json"), storage);
    ASSERT_TRUE(delta.has_value());
    // THE MEASUREMENT, pinned: U equals the PREVIOUS frame's u, not u + 1.
    EXPECT_EQ(delta->first_id, first_u);
    EXPECT_NE(delta->first_id, first_u + 1);

    const auto next = parse_depth(fixture("ws_depth_update_next.json"), storage);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->first_id, delta->last_id);
}

TEST(WeexBook, ASnapshotSeedsAndTheDeltasChainOntoIt) {
    const auto table = load_table();
    const auto& btc = listed(table, "BTCUSDT");
    DepthBook book{btc.id, btc.tick_size, btc.lot_size};

    static thread_local LevelBuffer storage;
    ASSERT_EQ(book.apply(*parse_depth(fixture("ws_depth_snapshot.json"), storage)),
              BookResult::Applied);
    EXPECT_TRUE(book.live());
    EXPECT_EQ(book.bids().size(), 15U);
    EXPECT_EQ(book.asks().size(), 15U);
    EXPECT_EQ(book.dropped_levels(), 0U);
    // The venue's own window, converted against the venue's own steps: tick
    // 0.1 and lot 0.0001, so 79349.8 is 793498 ticks and 2.8363 is 28363 lots.
    EXPECT_EQ(book.bids().front().price.raw, 793'498);
    EXPECT_EQ(book.bids().front().qty.raw, 28'363);
    EXPECT_EQ(book.asks().front().price.raw, 793'499);

    ASSERT_EQ(book.apply(*parse_depth(fixture("ws_depth_update.json"), storage)),
              BookResult::Applied);
    EXPECT_EQ(book.bids().front().qty.raw, 24'486);
    EXPECT_EQ(book.asks().front().qty.raw, 27'253);
    EXPECT_EQ(book.gaps(), 0U);
}

TEST(WeexBook, QuantityZeroDeletesALevelAndTheWindowStaysFifteen) {
    const auto table = load_table();
    const auto& btc = listed(table, "BTCUSDT");
    DepthBook book{btc.id, btc.tick_size, btc.lot_size};

    static thread_local LevelBuffer storage;
    ASSERT_EQ(book.apply(*parse_depth(fixture("ws_depth_snapshot.json"), storage)),
              BookResult::Applied);
    ASSERT_EQ(book.apply(*parse_depth(fixture("ws_depth_update.json"), storage)),
              BookResult::Applied);
    // 79346.5 was set to 0.4527 by the frame above; the next one sends it at
    // "0", which is how WEEX keeps the window at fifteen a side.
    const auto before = book.bids();
    EXPECT_NE(std::ranges::find_if(
                  before, [](const book::Level& level) { return level.price.raw == 793'465; }),
              before.end());

    ASSERT_EQ(book.apply(*parse_depth(fixture("ws_depth_update_next.json"), storage)),
              BookResult::Applied);
    const auto after = book.bids();
    EXPECT_EQ(std::ranges::find_if(
                  after, [](const book::Level& level) { return level.price.raw == 793'465; }),
              after.end());
    // One level in, one out: nothing here prunes because nothing needs to.
    EXPECT_EQ(after.size(), 15U);
    EXPECT_EQ(book.asks().size(), 15U);
}

TEST(WeexBook, TheTopOfBookIsTheOnlyQuoteThisVenueHas) {
    const auto table = load_table();
    const auto& btc = listed(table, "BTCUSDT");
    DepthBook book{btc.id, btc.tick_size, btc.lot_size};
    static thread_local LevelBuffer storage;

    // Nothing before the seed, because a book that cannot be published cannot
    // be quoted from either.
    EXPECT_FALSE(book.touch(k_ingest, k_ingest).has_value());

    ASSERT_EQ(book.apply(*parse_depth(fixture("ws_depth_snapshot.json"), storage)),
              BookResult::Applied);
    const auto bbo = book.touch(k_ingest, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->instrument.packed, btc.id.packed);
    EXPECT_EQ(bbo->bid.raw, 793'498);
    EXPECT_EQ(bbo->ask.raw, 793'499);
    EXPECT_EQ(bbo->bid_qty.raw, 28'363);
    EXPECT_EQ(bbo->ask_qty.raw, 26'383);
    EXPECT_LT(bbo->bid.raw, bbo->ask.raw);
}

TEST(WeexBook, ADeltaBeforeTheChannelsOwnSnapshotIsDroppedNotGuessed) {
    const auto table = load_table();
    const auto& btc = listed(table, "BTCUSDT");
    DepthBook book{btc.id, btc.tick_size, btc.lot_size};
    static thread_local LevelBuffer storage;

    EXPECT_EQ(book.apply(*parse_depth(fixture("ws_depth_update.json"), storage)),
              BookResult::Unseeded);
    EXPECT_FALSE(book.live());
    EXPECT_FALSE(book.snapshot(k_ingest, k_ingest).has_value());
}

TEST(WeexBook, ABrokenChainInvalidatesRatherThanServingAStaleBook) {
    const auto table = load_table();
    const auto& btc = listed(table, "BTCUSDT");
    DepthBook book{btc.id, btc.tick_size, btc.lot_size};
    static thread_local LevelBuffer storage;

    ASSERT_EQ(book.apply(*parse_depth(fixture("ws_depth_snapshot.json"), storage)),
              BookResult::Applied);
    // The MIDDLE frame is skipped, which is exactly what a dropped message
    // looks like: the next one's U names an id this book never reached.
    EXPECT_EQ(book.apply(*parse_depth(fixture("ws_depth_update_next.json"), storage)),
              BookResult::Gap);
    EXPECT_EQ(book.gaps(), 1U);
    EXPECT_FALSE(book.live());
    EXPECT_FALSE(book.snapshot(k_ingest, k_ingest).has_value());
    EXPECT_FALSE(book.touch(k_ingest, k_ingest).has_value());

    // A fresh snapshot is the whole recovery — the socket's next open re-asks
    // and WEEX answers the depth subscription with one.
    EXPECT_EQ(book.apply(*parse_depth(fixture("ws_depth_snapshot.json"), storage)),
              BookResult::Applied);
    EXPECT_TRUE(book.live());
}

TEST(WeexBook, AFrameAlreadyAppliedIsStaleRatherThanAGap) {
    const auto table = load_table();
    const auto& btc = listed(table, "BTCUSDT");
    DepthBook book{btc.id, btc.tick_size, btc.lot_size};
    static thread_local LevelBuffer storage;

    ASSERT_EQ(book.apply(*parse_depth(fixture("ws_depth_snapshot.json"), storage)),
              BookResult::Applied);
    ASSERT_EQ(book.apply(*parse_depth(fixture("ws_depth_update.json"), storage)),
              BookResult::Applied);
    EXPECT_EQ(book.apply(*parse_depth(fixture("ws_depth_update.json"), storage)),
              BookResult::Stale);
    EXPECT_EQ(book.gaps(), 0U); // a repeat must not read as a break
    EXPECT_TRUE(book.live());
}

TEST(WeexBook, ALevelThatDoesNotDivideByTheTickIsDroppedNeverRounded) {
    const auto table = load_table();
    const auto& btc = listed(table, "BTCUSDT");
    // BTCUSDT's real step is 0.1. This is the same book read as if WEEX had
    // moved pricePrecision from 1 to 0 — a schema change, and all but one level
    // of the frame then ends in a tenth the step cannot express. Rounding them
    // would move the whole ladder off the venue's own prices.
    DepthBook book{btc.id, FixedE9::from_raw(1'000'000'000), btc.lot_size};
    static thread_local LevelBuffer storage;

    EXPECT_EQ(book.apply(*parse_depth(fixture("ws_depth_snapshot.json"), storage)),
              BookResult::Unseeded);
    // 29 of 30, because one bid — 79348.0 — happens to sit on a whole unit.
    // What survives is a single price on one side, and a one-sided book is not
    // a book: it is published as nothing rather than as half a quote.
    EXPECT_EQ(book.dropped_levels(), 29U);
    EXPECT_FALSE(book.live());
}

// ------------------------------------------------------------ trades ----

TEST(WeexMap, TheMakerFlagIsTheAggressorInverted) {
    // Correlated live against the 15-level book: `m: true` printed at or below
    // the best bid 372 times and at or above the ask 33. Inverted, every
    // signed-flow series built on this venue runs backwards and says nothing.
    const auto table = load_table();
    static thread_local PrintBuffer storage;

    const auto buys = parse_trades(fixture("ws_trade_buy.json"), storage);
    ASSERT_TRUE(buys.has_value());
    ASSERT_EQ(buys->size(), 2U);
    for (const auto& print : *buys) {
        EXPECT_FALSE(print.buyer_is_maker);
        const auto trade = map_trade(print, "BTCUSDT", table, k_ingest);
        ASSERT_TRUE(trade.has_value());
        EXPECT_EQ(trade->aggressor, Side::Buy);
    }

    const auto sells = parse_trades(fixture("ws_trade_sell.json"), storage);
    ASSERT_TRUE(sells.has_value());
    ASSERT_EQ(sells->size(), 3U);
    for (const auto& print : *sells) {
        EXPECT_TRUE(print.buyer_is_maker);
        const auto trade = map_trade(print, "DOGEUSDT", table, k_ingest);
        ASSERT_TRUE(trade.has_value());
        EXPECT_EQ(trade->aggressor, Side::Sell);
    }
}

TEST(WeexMap, ATradeIdIsAUuidAndNotANumber) {
    // Read as an integer it would be zero on every print, and the reasoning
    // behind dropping the trade backfill is a statement about these ids.
    static thread_local PrintBuffer storage;
    const auto prints = parse_trades(fixture("ws_trade_buy.json"), storage);
    ASSERT_TRUE(prints.has_value());
    EXPECT_EQ((*prints)[0].trade_id, "b6e4d7bc-dff4-4f21-acc8-2976c16746d2");
    EXPECT_EQ((*prints)[0].trade_ms, 1'788'774'085'716ULL);
}

TEST(WeexMap, EachPrintCarriesItsOwnTimeAndNotTheFramesSendTime) {
    // One frame carries several prints and they did not all happen when WEEX
    // sent them: these two are 406 ms apart inside one message.
    const auto table = load_table();
    static thread_local PrintBuffer storage;
    const auto prints = parse_trades(fixture("ws_trade_buy.json"), storage);
    ASSERT_TRUE(prints.has_value());
    const auto first = map_trade((*prints)[0], "BTCUSDT", table, k_ingest);
    const auto second = map_trade((*prints)[1], "BTCUSDT", table, k_ingest);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_NE(first->ts_event.ns, second->ts_event.ns);
    EXPECT_EQ(first->ts_event.ns, 1'788'774'085'716'000'000ULL);
    EXPECT_EQ(first->ts_init.ns, k_ingest.ns);
}

TEST(WeexMap, ASizeOnANegativePrecisionContractDividesByItsStatedStep) {
    // DOGEUSDT's quantityPrecision is -2, so its size steps in HUNDREDS. An
    // exponent computed as if the precision were positive would put the step
    // four orders of magnitude out and refuse every print the venue sends.
    const auto table = load_table();
    const auto& doge = listed(table, "DOGEUSDT");
    EXPECT_EQ(doge.lot_size.raw, 100'000'000'000); // 100, in e9
    EXPECT_EQ(doge.tick_size.raw, 10'000);         // 1e-5, in e9

    static thread_local PrintBuffer storage;
    const auto prints = parse_trades(fixture("ws_trade_sell.json"), storage);
    ASSERT_TRUE(prints.has_value());
    const auto trade = map_trade((*prints)[0], "DOGEUSDT", table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->qty.raw, 15);      // "1500" / 100
    EXPECT_EQ(trade->price.raw, 8'956); // "0.08956" / 1e-5
}

// ------------------------------------------------------------ ticker ----

TEST(WeexMap, TheTickerCarriesBothTurnoverColumnsAndNoQuote) {
    const auto table = load_table();
    const auto wire = parse_ticker(fixture("ws_ticker.json"));
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "BTCUSDT");
    EXPECT_EQ(wire->volume, "15056.6200");
    EXPECT_EQ(wire->quote_volume, "1199459641.66359");

    const auto stats = map_ticker_stats(*wire, table, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base.raw, 15'056'620'000'000);
    EXPECT_EQ(stats->volume_24h_quote.raw, 1'199'459'641'663'590'000);
    // WHICH COLUMN IS WHICH, arithmetically rather than by name. Both raws are
    // e9, so their ratio is a price: 79663, which is this frame's own weighted
    // average `w` of 79663.27 and inside its 78947.5..80531.0 day range. Swap
    // the two and the same division says one BTC costs nothing at all.
    //
    // It is also the proof that sizes on this venue are BASE amounts rather
    // than contract counts, which is why contract_value is one.
    EXPECT_EQ(stats->volume_24h_quote.raw / stats->volume_24h_base.raw, 79'663);
    // The window's close, not the send time.
    EXPECT_EQ(stats->ts_event.ns, 1'788'773'400'000'000'000ULL);
}

TEST(WeexMap, AColumnTheTickerDidNotStateIsUndefAndNotZero) {
    // EDITED, one field: every live frame carries both columns, which is why
    // the rule needs a test. A zero says the market turned over nothing; an
    // UNDEF says WEEX did not say, and a board that draws the first when the
    // second is true reports a dead market.
    const auto table = load_table();
    const auto edited = without(fixture("ws_ticker.json"), R"("q":"1199459641.66359",)");
    const auto wire = parse_ticker(edited);
    ASSERT_TRUE(wire.has_value());
    EXPECT_TRUE(wire->quote_volume.empty());

    const auto stats = map_ticker_stats(*wire, table, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_FALSE(stats->volume_24h_base.is_undef());
    EXPECT_TRUE(stats->volume_24h_quote.is_undef());
    EXPECT_NE(stats->volume_24h_quote.raw, 0);
}

// ----------------------------------------------------------- funding ----

TEST(WeexMap, FundingAtZeroIsNotTheSameAsQuotingNoFunding) {
    const auto table = load_table();
    static thread_local PremiumBuffer storage;
    const auto rows = parse_premium_index(fixture("rest_premium_index.json"), storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);

    // USDCUSDT quotes "0.000" — a contract that funds at EXACTLY zero, which
    // is a fact about the market and not an absence of one.
    const auto zero = map_funding((*rows)[1], table, k_ingest);
    ASSERT_TRUE(zero.has_value());
    EXPECT_EQ((*rows)[1].symbol, "USDCUSDT");
    EXPECT_EQ(zero->rate.raw, 0);
    EXPECT_FALSE(zero->rate.is_undef());

    // EDITED, one field: no live row omits the rate, and every one of the 1022
    // states a number. A row that stated none must publish NOTHING rather than
    // a contract funding at zero.
    auto silent = (*rows)[0];
    silent.funding_rate = {};
    EXPECT_FALSE(map_funding(silent, table, k_ingest).has_value());
}

TEST(WeexMap, ARateFinerThanFixedPointIsTruncatedRatherThanRefused) {
    // GER40USDT states "0.00000001988" — eleven fractional digits, and 349 of
    // 1022 rows are padded past 1e-9 like it. An exact read refuses every one
    // of them and presents a funding contract as silent.
    const auto table = load_table();
    static thread_local PremiumBuffer storage;
    const auto rows = parse_premium_index(fixture("rest_premium_index.json"), storage);
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ((*rows)[2].symbol, "GER40USDT");
    EXPECT_EQ((*rows)[2].funding_rate, "0.00000001988");

    const auto funding = map_funding((*rows)[2], table, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 19); // toward zero, not away from it
    EXPECT_EQ(funding->predicted_rate.raw, 0);
}

TEST(WeexMap, TheMarkIsCarriedExactlyAndTheIndexNotAtAll) {
    const auto table = load_table();
    static thread_local PremiumBuffer storage;
    const auto rows = parse_premium_index(fixture("rest_premium_index.json"), storage);
    ASSERT_TRUE(rows.has_value());

    const auto btc = map_funding((*rows)[0], table, k_ingest);
    ASSERT_TRUE(btc.has_value());
    EXPECT_EQ(btc->rate.raw, 31'160);           // "0.00003116"
    EXPECT_EQ(btc->predicted_rate.raw, 28'960); // "0.00002896"
    EXPECT_EQ(btc->mark_price.raw, 794'214);    // "79421.4" at a 0.1 tick
    EXPECT_EQ(btc->next_funding_time.ns, 1'788'796'800'000'000'000ULL);

    // THEUSDT's index is "0.07308125" against a 1e-5 tick — 7308.125 steps,
    // which is not a number of steps at all. It is the reason no index price
    // is carried anywhere in this adapter: 446 of 1022 rows are like this one,
    // and the only way to file it as a PriceTicks is to round it into a price
    // WEEX never quoted. Its MARK divides, which is why that one is kept.
    const auto& the = listed(table, "THEUSDT");
    EXPECT_EQ(the.tick_size.raw, 10'000);
    const auto the_funding = map_funding((*rows)[3], table, k_ingest);
    ASSERT_TRUE(the_funding.has_value());
    EXPECT_EQ(the_funding->mark_price.raw, 7'323); // "0.07323" divides
    EXPECT_EQ((*rows)[3].symbol, "THEUSDT");

    // EDITED, one field: every live row states a mark. One that did not, or
    // one that stated a price off the tick, must leave the field UNDEF — a
    // basis measured against a rounded mark is wrong by the rounding.
    auto markless = (*rows)[0];
    markless.mark_price = "79421.45"; // half a tick, which WEEX cannot quote
    const auto refused = map_funding(markless, table, k_ingest);
    ASSERT_TRUE(refused.has_value());
    EXPECT_TRUE(refused->mark_price.is_undef());
    EXPECT_NE(refused->mark_price.raw, 0);
}

TEST(WeexMap, TheFundingIntervalComesFromTheCataloguesSettlementTimetable) {
    // premiumIndex states collectCycle in minutes and the catalogue states the
    // same fact as a list of settlement times; they agreed on all 1022 rows.
    // The catalogue is what is read, because the instrument carries the
    // interval and a funding update should not need a second source for it.
    const auto table = load_table();
    static thread_local PremiumBuffer storage;
    const auto rows = parse_premium_index(fixture("rest_premium_index.json"), storage);
    ASSERT_TRUE(rows.has_value());

    const auto btc = map_funding((*rows)[0], table, k_ingest);
    ASSERT_TRUE(btc.has_value());
    EXPECT_EQ(btc->interval_s, 28'800U); // three settlements a day
    const auto the = map_funding((*rows)[3], table, k_ingest);
    ASSERT_TRUE(the.has_value());
    EXPECT_EQ(the->interval_s, 14'400U); // six
}

// --------------------------------------------------------- catalogue ----

TEST(WeexCatalogue, EveryRowIsolatesOneMapperDecision) {
    static thread_local InstrumentBuffer storage;
    const auto rows = parse_exchange_info(fixture("rest_exchange_info.json"), storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 8U);

    struct Expected {
        std::string_view symbol;
        bool accepted;
        std::int64_t tick;
        std::int64_t lot;
        std::uint32_t interval_s;
    };

    // 86400 / the length of the `delivery` timetable, which is the only thing
    // read out of that array.
    const std::array<Expected, 8> table{{
        {"BTCUSDT", true, 100'000'000, 100'000, 28'800},
        {"DOGEUSDT", true, 10'000, 100'000'000'000, 28'800},
        {"EURUSDT", true, 100'000, 1'000'000'000, 14'400},
        {"ONGUSDT", true, 10'000, 10'000'000'000, 3'600},
        {"THEUSDT", true, 10'000, 1'000'000'000, 14'400},
        {"USDCUSDT", true, 1'000, 1'000'000'000, 28'800},
        {"GER40USDT", true, 10'000'000, 100'000, 28'800},
        // pricePrecision 10 is a step of 1e-10, finer than fixed point holds.
        // Two of the 1022 live rows are like this, and a step nothing divides
        // exactly would drop every level of every frame for the contract.
        {"QUBICUSDT", false, 0, 0, 0},
    }};
    for (std::size_t i = 0; i < table.size(); ++i) {
        const auto& want = table[i];
        ASSERT_EQ((*rows)[i].symbol, want.symbol);
        const auto mapped = map_instrument((*rows)[i], MarketType::Perp);
        ASSERT_EQ(mapped.has_value(), want.accepted) << want.symbol;
        if (!want.accepted) {
            continue;
        }
        const auto& base = base_of(*mapped);
        EXPECT_EQ(base.tick_size.raw, want.tick) << want.symbol;
        EXPECT_EQ(base.lot_size.raw, want.lot) << want.symbol;
        EXPECT_EQ(std::get<PerpInstrument>(*mapped).funding_interval_s, want.interval_s)
            << want.symbol;
    }
}

TEST(WeexCatalogue, ASizeStepIsReadAsADecimalAndNeverAsAnExponent) {
    // contractVal equalled 10^-quantityPrecision on 1022 of 1022 rows, and
    // reading the decimal sidesteps the trap in the other field: 385 rows carry
    // a NEGATIVE quantityPrecision, down to -6, and an exponent computed as if
    // it were positive would put the step twelve orders of magnitude out.
    static thread_local InstrumentBuffer storage;
    const auto rows = parse_exchange_info(fixture("rest_exchange_info.json"), storage);
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ((*rows)[1].contract_val, "100");    // DOGEUSDT, precision -2
    EXPECT_EQ((*rows)[0].contract_val, "0.0001"); // BTCUSDT, precision 4
    // The same body writes the number as an integer on one row and as a float
    // on the next, which is why it is taken as raw text and parsed once.
    EXPECT_EQ((*rows)[3].contract_val, "10"); // ONGUSDT, precision -1
}

TEST(WeexCatalogue, TradifiPerpetualsAreAdmittedAndPricedLikeAnyOther) {
    // 446 of the 1022 rows are TRADIFI_PERPETUAL — perpetuals on FX, metals and
    // equity indices. They settle, fund and margin in USDT exactly as the
    // others do, so refusing them would drop 44 % of the catalogue over what
    // their index tracks.
    static thread_local InstrumentBuffer storage;
    const auto rows = parse_exchange_info(fixture("rest_exchange_info.json"), storage);
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ((*rows)[2].contract_type, "TRADIFI_PERPETUAL");
    const auto eur = map_instrument((*rows)[2], MarketType::Perp);
    ASSERT_TRUE(eur.has_value());
    EXPECT_EQ(base_of(*eur).local_symbol.view(), "EUR/USDT");
    EXPECT_EQ(base_of(*eur).kind, ContractKind::Linear);
}

TEST(WeexCatalogue, WhatWeexDidNotStateStaysUndef) {
    // WEEX states NO minimum notional. `minOrderSize` is a base-asset QUANTITY
    // — it equals the size step on 1020 of 1022 rows — and filing a quantity in
    // a notional field would have the order gate comparing a coin count against
    // a dollar figure. The fee rates it does state are the public tier's, which
    // is not necessarily what an account pays.
    static thread_local InstrumentBuffer storage;
    const auto rows = parse_exchange_info(fixture("rest_exchange_info.json"), storage);
    ASSERT_TRUE(rows.has_value());
    const auto btc = map_instrument((*rows)[0], MarketType::Perp);
    ASSERT_TRUE(btc.has_value());
    const auto& base = base_of(*btc);
    EXPECT_TRUE(base.min_notional.is_undef());
    EXPECT_TRUE(base.maker_fee.is_undef());
    EXPECT_TRUE(base.taker_fee.is_undef());
    // And the row still validates, because none of those is what makes an
    // instrument tradable.
    EXPECT_TRUE(validate(base));
    // One base unit per unit of size: quantities on this tree are base amounts,
    // not contract counts.
    EXPECT_EQ(base.contract_value.raw, 1'000'000'000);
}

TEST(WeexCatalogue, OnlyPerpIsLoaded) {
    static thread_local InstrumentBuffer storage;
    const auto rows = parse_exchange_info(fixture("rest_exchange_info.json"), storage);
    ASSERT_TRUE(rows.has_value());
    for (const auto market : {MarketType::Spot, MarketType::Future, MarketType::Option}) {
        EXPECT_FALSE(map_instrument((*rows)[0], market).has_value()) << to_string(market);
    }
}

// ---------------------------------------------------------------- ws ----

TEST(WeexWs, BboAndDepthNameOneChannel) {
    // WEEX streams no quote channel — @bookTicker is refused by name — so the
    // top of book comes off the ladder. A gateway asking for both kinds must
    // spend ONE of the connection's hundred channels, not two.
    std::array<char, 64> quote{};
    std::array<char, 64> ladder{};
    const auto bbo = channel_name(exec::SubKind::Bbo, "BTCUSDT", quote);
    const auto depth = channel_name(exec::SubKind::Depth, "BTCUSDT", ladder);
    EXPECT_EQ(bbo, "BTCUSDT@depth15");
    EXPECT_EQ(bbo, depth);
}

TEST(WeexWs, TheSymbolGoesOnTheWireAsTheCatalogueSpelledIt) {
    // Both halves of a channel are case-insensitive live, so nothing folds
    // case and the exchange symbol travels unchanged.
    std::array<char, 64> buf{};
    EXPECT_EQ(channel_name(exec::SubKind::Trades, "DOGEUSDT", buf), "DOGEUSDT@trade");
    EXPECT_EQ(channel_name(exec::SubKind::MarkPrice, "BTCUSDT", buf), "BTCUSDT@ticker");
    // Funding is a REST poll, not a channel; the data client routes the kind
    // before it reaches here.
    EXPECT_TRUE(channel_name(exec::SubKind::Funding, "BTCUSDT", buf).empty());
    EXPECT_TRUE(channel_name(exec::SubKind::Kline, "BTCUSDT", buf).empty());
}

TEST(WeexWs, ASubscribeFrameBatchesAndReportsWhatItConsumed) {
    Protocol protocol;
    const std::array<std::string_view, 2> two{"BTCUSDT@depth15", "BTCUSDT@trade"};
    const auto frame = protocol.subscribe_frame(two);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->consumed, 2U);
    EXPECT_EQ(frame->text,
              R"({"method":"SUBSCRIBE","params":["BTCUSDT@depth15","BTCUSDT@trade"],"id":1})");

    std::vector<std::string_view> many(k_max_channels_per_frame + 5, "BTCUSDT@trade");
    const auto batched = protocol.subscribe_frame(many);
    ASSERT_TRUE(batched.has_value());
    EXPECT_EQ(batched->consumed, k_max_channels_per_frame);
}

TEST(WeexWs, AnEmptyChannelIsRefusedRatherThanTakingTheFrameDownWithIt) {
    // WEEX refuses a params array for ONE bad member and subscribes none of the
    // rest: a single invalid name in an array of eight left all eight unasked
    // and pushing nothing. So a name nothing could build is caught here.
    Protocol protocol;
    const std::array<std::string_view, 2> poisoned{"BTCUSDT@depth15", {}};
    const auto frame = protocol.subscribe_frame(poisoned);
    ASSERT_FALSE(frame.has_value());
    EXPECT_EQ(frame.error(), WsBuildError::UnsupportedKind);
    EXPECT_FALSE(protocol.subscribe_frame({}).has_value());
}

TEST(WeexWs, ThePoolHoldsTheWholeCatalogueWithoutPassingTheIpCap) {
    // The two ceilings WEEX states — 100 channels a connection, 20 connections
    // an IP — multiply to 2000, and its own catalogue needs 3060: three
    // channels for each of 1020 contracts. Held to both, this gateway
    // subscribed 534 and 486 contracts stayed blank. So the test is not that
    // the constants match the page, which pinned the failure in place; it is
    // that the pool covers the catalogue and stops short of the cap that would
    // actually cost us the connection.
    constexpr std::size_t k_catalogue_channels = 1020U * 3U;
    EXPECT_GE(k_max_channels_per_connection * k_max_connections, k_catalogue_channels);
    EXPECT_LT(k_max_connections, 20U);
    EXPECT_LE(k_max_channels_per_frame, k_max_channels_per_connection);
    // Server-driven keepalive: WEEX does not echo the ping's timestamp, so the
    // answer is a constant rather than a frame built per ping.
    EXPECT_EQ(k_pong_frame, R"({"method":"PONG","id":1})");
}

TEST(WeexWs, EveryAddressIsSpelledOnceAndTheBuilderUsesIt) {
    EXPECT_EQ(stream_host(), ep::k_stream_host);
    EXPECT_EQ(stream_path(), ep::k_ws_public);
    EXPECT_EQ(rest_host(), ep::k_rest_host);

    RequestBuilder builder;
    EXPECT_EQ(builder.instruments().target, ep::k_exchange_info);
    // NO `symbol`: the bare path answers for every contract in one body.
    EXPECT_EQ(builder.premium_index().target, ep::k_premium_index);
}

} // namespace
} // namespace raikiri::venues::weex
