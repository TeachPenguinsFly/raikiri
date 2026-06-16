#include <raikiri/net/signer.hpp>
#include <raikiri/venues/weex/account.hpp>
#include <raikiri/venues/weex/book.hpp>
#include <raikiri/venues/weex/catalogue.hpp>
#include <raikiri/venues/weex/endpoints.hpp>
#include <raikiri/venues/weex/envelope.hpp>
#include <raikiri/venues/weex/errors.hpp>
#include <raikiri/venues/weex/map.hpp>
#include <raikiri/venues/weex/market_data.hpp>
#include <raikiri/venues/weex/orders.hpp>
#include <raikiri/venues/weex/positions.hpp>
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

// A one-value edit, used only where the venue has never PRINTED the shape a
// rule exists to handle. Each caller says what it changed and why.
std::string replacing(std::string_view body, std::string_view from, std::string_view to) {
    std::string out{body};
    const auto at = out.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    out.replace(at, from.size(), to);
    return out;
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

TEST(WeexVenue, TheCapsRowClaimsOnlyWhatALiveCredentialHasProven) {
    // THIS TEST USED TO BE ALL FALSE, and it was right to be: every private
    // path was implemented against the venue's documented bodies and none had
    // ever been answered. A key reached this venue on 2026-09-15, each path was
    // run against it, and the bits below moved one at a time as a real body
    // came back for each — a position opened and closed, an order rested and
    // was cancelled by its ULID, a balance that tracked the margin, thirteen
    // funding settlements.
    //
    // WHAT IS STILL FALSE IS STILL UNTRIED, and that is the point now. A bit
    // set true on an unexercised path makes a reconciler trust an answer nobody
    // has seen; caps.cpp names, bit by bit, what evidence would flip each one.
    const auto& caps = exec::caps_of(VenueId::Weex);
    EXPECT_TRUE(caps.markets.perp);
    // Not because spot is another company — it is the same exchange behind the
    // same key — but because it is another host serving another path prefix and
    // nothing here addresses it.
    EXPECT_FALSE(caps.markets.spot);
    EXPECT_FALSE(caps.markets.future);
    EXPECT_FALSE(caps.markets.option);
    EXPECT_FALSE(caps.ws_order_entry);
    EXPECT_FALSE(caps.fix_order_entry);
    // Cancelled live by a ULID this process minted; the venue answered success
    // and dropped the order from its own list.
    EXPECT_TRUE(caps.cancel_by_client_order_id);
    // Still false, and for the ordinary reason: nobody has sent one. Ten ids at
    // a time is what the execution client chunks to, and no batch has run.
    EXPECT_FALSE(caps.batch_cancel);
    // This one is false for a DIFFERENT reason, and the difference matters: WEEX
    // publishes no modify for a resting order at all, so no credential would
    // ever flip it.
    EXPECT_FALSE(caps.amend_order);
    // Untried: it is a time in force on the create body here, and no order has
    // carried it.
    EXPECT_FALSE(caps.native_post_only);
    EXPECT_TRUE(caps.position_pull.has(MarketType::Perp));
    EXPECT_TRUE(caps.order_pull.has(MarketType::Perp));
    EXPECT_TRUE(caps.fill_pull.has(MarketType::Perp));
    EXPECT_TRUE(caps.balance_pull.has(MarketType::Perp));
    EXPECT_TRUE(caps.funding_charge.has(MarketType::Perp));
    // PERP AND NOTHING ELSE. The claim is per market, and a bit that leaked
    // into spot would have seven gateways signing calls at a venue that serves
    // this account no spot at all.
    EXPECT_FALSE(caps.position_pull.has(MarketType::Spot));
    EXPECT_FALSE(caps.order_pull.has(MarketType::Spot));
    EXPECT_FALSE(caps.balance_pull.has(MarketType::Spot));
    // WEEX publishes a private socket; this adapter does not open one, so a
    // fill reaches this process through the reconciliation pull and nowhere
    // else.
    EXPECT_EQ(caps.user_stream, exec::UserStreamModel::None);
    // The one public capability that IS claimed, and the ticker channel is
    // what delivers it.
    EXPECT_TRUE(caps.ticker_stats);

    // These two are facts about the VENUE rather than claims about this
    // adapter, and both are stated in its documentation: three credentials, and
    // 36 characters in `newClientOrderId`.
    EXPECT_TRUE(caps.needs_passphrase);
    EXPECT_EQ(caps.max_client_order_id_len, k_max_client_order_id);
    EXPECT_GE(caps.max_client_order_id_len, exec::k_min_client_order_id_len);
    // NOT STATED IN ANY UNIT THIS FIELD CAN CARRY. WEEX publishes 300 orders a
    // MINUTE per account and no per-second figure anywhere, so this stays zero
    // rather than holding a quotient the venue never quoted.
    EXPECT_EQ(caps.headline_order_rate_per_s, 0U);
}

TEST(WeexVenue, TwoOfThreeCredentialsIsRefusedAtStartupRatherThanPerOrder) {
    // WEEX signs with a passphrase beside the key and the secret, and without
    // one it answers -1049 — which is not the code a missing key produces and
    // reads as a revoked key for as long as nobody looks. The caps row's
    // needs_passphrase is what turns that into a refusal at load.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto refused = exec::make_venue({.venue = VenueId::Weex,
                                           .market = MarketType::Perp,
                                           .api_key_env = "env:WEEX_API_KEY",
                                           .api_secret_env = "env:WEEX_API_SECRET"},
                                          sink,
                                          clock);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), exec::MakeError::PassphraseRequired);

    // A gateway that names NO credential is asking for market data and is
    // refused nothing.
    EXPECT_TRUE(exec::make_venue({.venue = VenueId::Weex, .market = MarketType::Perp}, sink, clock)
                    .has_value());
}

TEST(WeexVenue, WithNoCredentialEveryCommandIsRefusedLocallyRatherThanLeftOpen) {
    // Silence is the one answer an order may never get: a command that vanishes
    // sits open in the store forever, indistinguishable from one the venue is
    // still thinking about. Nothing here reaches a socket — the request cannot
    // be built, let alone signed — so every verdict below is local and
    // immediate.
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
    EXPECT_EQ(stats->volume_24h_quote.raw, 1'199'459'641'663'590);
    // WHICH COLUMN IS WHICH, arithmetically rather than by name. Both raws are
    // e9, so their ratio is a price: 79663, which is this frame's own weighted
    // average `w` of 79663.27 and inside its 78947.5..80531.0 day range. Swap
    // the two and the same division says one BTC costs nothing at all.
    //
    // It is also the proof that sizes on this venue are BASE amounts rather
    // than contract counts, which is why contract_value is one.
    EXPECT_EQ(stats->volume_24h_quote.raw * (k_e9 / k_e6) / stats->volume_24h_base.raw, 79'663);
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

TEST(WeexCatalogue, TheSetAnOrderMayNameIsNotTheSetTheVenueLists) {
    // 290 of 1000 on 2026-09-15. Everything outside it answers -1058 to any
    // order — including all five contracts WEEX funds hourly, which is how this
    // was found: an order for one came back refused by the venue with a message
    // that reads like a defect in this adapter.
    //
    // A BARE ARRAY OF NAMES, with no envelope around it, unlike every other
    // body this venue serves.
    static thread_local TradableBuffer storage;
    const auto names = parse_api_trading_symbols(fixture("rest_api_trading_symbols.json"), storage);
    ASSERT_TRUE(names.has_value());
    ASSERT_EQ(names->size(), 5U);
    EXPECT_EQ((*names)[0], "1000BONKUSDT");
    EXPECT_EQ((*names)[2], "BTCUSDT");
    EXPECT_EQ((*names)[4], "XRPUSDT");
}

TEST(WeexCatalogue, AnEmptyTradableSetRefusesNothing) {
    // The set is read once at start, and a read that fails leaves it empty.
    // Empty must mean UNKNOWN: refusing every order because a secondary call
    // did not answer would take the venue down over a missing list, when the
    // worst the venue itself does is refuse the ones that are not on it.
    static thread_local TradableBuffer storage;
    const auto empty = parse_api_trading_symbols("[]", storage);
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ(empty->size(), 0U);
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

// ----------------------------------------------------------- private ----
//
// NOTHING BELOW HAS SEEN A LIVE SUCCESSFUL ANSWER. There are no WEEX
// credentials on this machine, so the fixtures these tests read are the venue's
// own documented bodies rather than captures, and they are labelled as such in
// fixtures/README.md. What IS measured is every refusal: the 401 and the 400
// were pulled off production unauthenticated on 2026-09-13, and they are the
// only proof this adapter has that its private paths are addresses at all.

// A signer holding test credentials. The secret is not a WEEX secret and never
// was: what these tests pin is the MESSAGE the adapter signs, which is the half
// that cannot be checked against the venue without a key.
struct TestSigner {
    net::Signer signer;

    TestSigner() {
        signer.set_api_key("weex-test-key");
        EXPECT_TRUE(signer.secret().assign("weex-test-secret").has_value());
    }
};

constexpr std::string_view k_passphrase = "weextestpass";
constexpr std::string_view k_ulid = "01JQ8ZK9F4M2N7P5R3T1V6W8XA";
constexpr UnixNanos k_signed_at{1'764'505'800'123'000'000ULL};
constexpr std::string_view k_signed_at_ms = "1764505800123";

[[nodiscard]] std::string_view header_of(const net::RestRequest& request, std::string_view name) {
    for (const auto& header : request.headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return {};
}

[[nodiscard]] OrderCmd limit_order(const exec::InstrumentTable& table) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.instrument = listed(table, "BTCUSDT").id;
    cmd.side = Side::Buy;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.client_order_id = ClientOrderId::make(1, 42);
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from(k_ulid);
    cmd.qty = QtyLots::from_raw(200);          // 0.02 BTC against a 0.0001 lot
    cmd.price = PriceTicks::from_raw(690'000); // 69000 against a 0.1 tick
    return cmd;
}

TEST(WeexSign, TheSignedMessageIsTimestampMethodPathQueryBody) {
    // The venue's own rule, and the one thing about this adapter that cannot be
    // checked against WEEX without a key: a GET that signs a bare path while
    // sending a query, or a POST that signs a query it never sends, produces a
    // digest wrong by bytes nobody can see — and WEEX answers both with the
    // generic refusal a revoked key draws.
    TestSigner credentials;
    RequestBuilder builder{credentials.signer, k_passphrase};

    const auto get = builder.open_orders(MarketType::Perp, k_signed_at);
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ(builder.last_sign_message(),
              std::string{k_signed_at_ms} + "GET" + std::string{ep::k_open_orders} +
                  "?limit=100&page=0");
    // The "?" belongs to the signer and appears exactly once, in the message and
    // in the target alike.
    EXPECT_EQ(get->target, std::string{ep::k_open_orders} + "?limit=100&page=0");

    const auto table = load_table();
    const auto post = builder.create_order(limit_order(table), table, k_signed_at);
    ASSERT_TRUE(post.has_value());
    const auto message = std::string{builder.last_sign_message()};
    EXPECT_EQ(message,
              std::string{k_signed_at_ms} + "POST" + std::string{ep::k_order} +
                  std::string{post->body});
    // AN EMPTY QUERY TAKES NO "?" AT ALL, which is the half of the rule a
    // careless signer gets wrong in the other direction.
    EXPECT_EQ(message.find('?'), std::string::npos);
}

TEST(WeexSign, TheHeaderCarriesABase64DigestOfExactlyThatMessage) {
    TestSigner credentials;
    RequestBuilder builder{credentials.signer, k_passphrase};

    const auto request = builder.positions(MarketType::Perp, k_signed_at);
    ASSERT_TRUE(request.has_value());

    std::array<char, net::k_base64_bytes> expected{};
    const auto digest =
        net::hmac_sha256_base64("weex-test-secret", builder.last_sign_message(), expected);
    EXPECT_EQ(header_of(*request, "ACCESS-SIGN"), digest);
    // BASE64, not the hex most of this fleet uses: 32 digest bytes become 44
    // characters ending in the one pad the length forces.
    EXPECT_EQ(digest.size(), net::k_base64_bytes);
    EXPECT_EQ(digest.back(), '=');

    EXPECT_EQ(header_of(*request, "ACCESS-KEY"), "weex-test-key");
    EXPECT_EQ(header_of(*request, "ACCESS-TIMESTAMP"), k_signed_at_ms);
    EXPECT_EQ(header_of(*request, "ACCESS-PASSPHRASE"), k_passphrase);
    // The venue answers -1045 to anything else, on a GET with no body as much
    // as on a POST with one.
    EXPECT_EQ(request->content_type, "application/json");
}

TEST(WeexSign, TheDigestFollowsTheREQUESTAndNotTheOtherWayRound) {
    // A signature that did not change with the request would authenticate one
    // call for another — the failure a constant or a stale buffer produces, and
    // the one a venue cannot tell us about.
    TestSigner credentials;
    RequestBuilder builder{credentials.signer, k_passphrase};

    ASSERT_TRUE(builder.positions(MarketType::Perp, k_signed_at).has_value());
    const std::string positions{builder.last_sign_message()};
    ASSERT_TRUE(builder.balances(k_signed_at).has_value());
    const std::string balances{builder.last_sign_message()};
    EXPECT_NE(positions, balances);

    // And the same request at the same millisecond signs the same bytes: a
    // digest that wandered would make every retry a new request.
    ASSERT_TRUE(builder.positions(MarketType::Perp, k_signed_at).has_value());
    EXPECT_EQ(builder.last_sign_message(), positions);
}

TEST(WeexSign, HalfACredentialIsNoCredential) {
    // WEEX signs with THREE, and a key and secret without a passphrase answer
    // -1049 rather than the code a missing key produces — which reads as a
    // revoked key for as long as nobody looks. Refused before the request
    // exists instead.
    TestSigner credentials;
    RequestBuilder without_passphrase{credentials.signer, {}};
    const auto refused = without_passphrase.positions(MarketType::Perp, k_signed_at);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), BuildError::NotSigned);

    // And a builder holding nothing at all — the market-data half — refuses the
    // same way rather than signing with an empty key.
    RequestBuilder unsigned_builder;
    EXPECT_FALSE(unsigned_builder.balances(k_signed_at).has_value());
    // Its public calls still work, which is the whole point of the split.
    EXPECT_EQ(unsigned_builder.instruments().target, ep::k_exchange_info);
}

TEST(WeexSign, EveryPrivatePathAddressesTheContractHost) {
    // WEEX's spot exchange is api-spot.weex.com serving /api/v3/..., and this
    // host serves /capi/v3/... — neither answers the other's paths, verified
    // live on 2026-09-13 (both cross-host reads were 404). So a private path
    // that lost its `capi` would be a spot call made with contract permissions.
    EXPECT_EQ(rest_host(), "api-contract.weex.com");
    for (const auto path : {ep::k_order,
                            ep::k_batch_orders,
                            ep::k_open_orders,
                            ep::k_user_trades,
                            ep::k_all_positions,
                            ep::k_balance,
                            ep::k_income,
                            ep::k_server_time}) {
        EXPECT_TRUE(path.starts_with("/capi/v3/")) << path;
    }
}

TEST(WeexOrders, TheUlidTravelsUnchangedAndFitsTheVenuesField) {
    // WEEX states 1-36 characters matching ^[\.A-Z\:/a-z0-9_-]{1,36}$ on
    // `newClientOrderId`. A 26-character Crockford ULID is digits and upper-case
    // letters, so it fits both and needs no re-encoding — and an id that
    // arrived re-encoded would come back as something this process cannot
    // recognise.
    static_assert(k_max_client_order_id >= 26);
    TestSigner credentials;
    RequestBuilder builder{credentials.signer, k_passphrase};
    const auto table = load_table();

    const auto request = builder.create_order(limit_order(table), table, k_signed_at);
    ASSERT_TRUE(request.has_value());
    EXPECT_NE(request->body.find(R"("newClientOrderId":"01JQ8ZK9F4M2N7P5R3T1V6W8XA")"),
              std::string_view::npos)
        << request->body;
    EXPECT_LE(k_ulid.size(), k_max_client_order_id);
}

TEST(WeexOrders, ARequiredFieldTheVenueWouldRefuseIsRefusedHere) {
    TestSigner credentials;
    RequestBuilder builder{credentials.signer, k_passphrase};
    const auto table = load_table();

    // UNDEF IS NOT ZERO. A quantity the OMS never filled in must not be
    // rendered as "0": WEEX would refuse it on a good day and take it on a bad
    // one.
    auto no_qty = limit_order(table);
    no_qty.qty = QtyLots::undef();
    ASSERT_FALSE(builder.create_order(no_qty, table, k_signed_at).has_value());
    EXPECT_EQ(builder.create_order(no_qty, table, k_signed_at).error(), BuildError::BadValue);

    auto no_price = limit_order(table);
    no_price.price = PriceTicks::undef();
    EXPECT_EQ(builder.create_order(no_price, table, k_signed_at).error(), BuildError::BadValue);

    // `newClientOrderId` is REQUIRED on this venue, and an order sent without
    // one would come back unattributable.
    auto no_id = limit_order(table);
    no_id.exchange_client_order_id = {};
    EXPECT_EQ(builder.create_order(no_id, table, k_signed_at).error(), BuildError::NoOrderTarget);

    // The contract tree and nothing else, answered before a request exists.
    EXPECT_EQ(builder.positions(MarketType::Spot, k_signed_at).error(),
              BuildError::UnsupportedMarket);
}

TEST(WeexOrders, AReduceOnlyOrderFacesThePositionItCloses) {
    // WEEX requires a positionSide on every order and its vocabulary is LONG
    // and SHORT with no "both", so the field is DERIVED — and this is the most
    // likely thing in the adapter to be wrong the first time a credential
    // reaches the venue. An opening order faces the way it trades; a
    // reduce-only order faces the position it is closing, which is the
    // opposite.
    TestSigner credentials;
    RequestBuilder builder{credentials.signer, k_passphrase};
    const auto table = load_table();

    const auto side_in_body = [&](Side side, OrderFlags flags) {
        auto cmd = limit_order(table);
        cmd.side = side;
        cmd.flags = flags;
        const auto request = builder.create_order(cmd, table, k_signed_at);
        EXPECT_TRUE(request.has_value());
        return std::string{request.has_value() ? request->body : std::string_view{}};
    };

    EXPECT_NE(side_in_body(Side::Buy, OrderFlags::None).find(R"("positionSide":"LONG")"),
              std::string::npos);
    EXPECT_NE(side_in_body(Side::Sell, OrderFlags::None).find(R"("positionSide":"SHORT")"),
              std::string::npos);
    EXPECT_NE(side_in_body(Side::Buy, OrderFlags::ReduceOnly).find(R"("positionSide":"SHORT")"),
              std::string::npos);
    EXPECT_NE(side_in_body(Side::Sell, OrderFlags::ReduceOnly).find(R"("positionSide":"LONG")"),
              std::string::npos);
    // And the flag itself still travels, because positionSide alone does not
    // stop an order opening the opposite leg.
    EXPECT_NE(side_in_body(Side::Buy, OrderFlags::ReduceOnly).find(R"("reduceOnly":true)"),
              std::string::npos);
}

TEST(WeexOrders, PostOnlyIsATimeInForceAndAMarketOrderCarriesNone) {
    // Sending post-only as a boolean is ignored on this venue and the order
    // crosses; timeInForce is where it lives.
    TestSigner credentials;
    RequestBuilder builder{credentials.signer, k_passphrase};
    const auto table = load_table();

    auto maker = limit_order(table);
    maker.flags = OrderFlags::PostOnly;
    const auto post_only = builder.create_order(maker, table, k_signed_at);
    ASSERT_TRUE(post_only.has_value());
    EXPECT_NE(post_only->body.find(R"("timeInForce":"POST_ONLY")"), std::string_view::npos);
    EXPECT_EQ(post_only->body.find("postOnly"), std::string_view::npos);

    auto ioc = limit_order(table);
    ioc.tif = TimeInForce::Ioc;
    const auto immediate = builder.create_order(ioc, table, k_signed_at);
    ASSERT_TRUE(immediate.has_value());
    EXPECT_NE(immediate->body.find(R"("timeInForce":"IOC")"), std::string_view::npos);

    // `timeInForce` and `price` are required only for a LIMIT, and a market
    // order carrying either is a request this venue has no reason to accept.
    auto market = limit_order(table);
    market.order_type = OrderType::Market;
    const auto taker = builder.create_order(market, table, k_signed_at);
    ASSERT_TRUE(taker.has_value());
    EXPECT_NE(taker->body.find(R"("type":"MARKET")"), std::string_view::npos);
    EXPECT_EQ(taker->body.find("timeInForce"), std::string_view::npos);
    EXPECT_EQ(taker->body.find(R"("price")"), std::string_view::npos);
}

TEST(WeexOrders, ACancelNamesTheVenuesOwnIdWhereItHasOneAndTheUlidOtherwise) {
    TestSigner credentials;
    RequestBuilder builder{credentials.signer, k_passphrase};
    const auto table = load_table();

    auto by_ulid = limit_order(table);
    by_ulid.kind = CmdKind::Cancel;
    const auto client = builder.cancel_order(by_ulid, k_signed_at);
    ASSERT_TRUE(client.has_value());
    EXPECT_EQ(client->method, net::HttpMethod::Delete);
    EXPECT_EQ(client->target,
              std::string{ep::k_order} + "?origClientOrderId=" + std::string{k_ulid});

    auto by_venue_id = by_ulid;
    by_venue_id.target_exchange_order_id = *ExchangeOrderId::from("702345678901234567");
    const auto venue = builder.cancel_order(by_venue_id, k_signed_at);
    ASSERT_TRUE(venue.has_value());
    EXPECT_EQ(venue->target, std::string{ep::k_order} + "?orderId=702345678901234567");

    // A cancel naming nothing is refused rather than sent as a cancel of
    // everything.
    auto nameless = by_ulid;
    nameless.exchange_client_order_id = {};
    EXPECT_EQ(builder.cancel_order(nameless, k_signed_at).error(), BuildError::NoOrderTarget);
}

TEST(WeexOrders, ABatchCancelNamesEveryOrderAndStopsAtTheVenuesTen) {
    TestSigner credentials;
    RequestBuilder builder{credentials.signer, k_passphrase};
    const auto table = load_table();

    std::vector<OrderCmd> batch;
    for (std::size_t i = 0; i < k_max_cancel_batch; ++i) {
        auto cmd = limit_order(table);
        cmd.kind = CmdKind::Cancel;
        std::string id{k_ulid};
        id.back() = static_cast<char>('A' + static_cast<char>(i));
        cmd.exchange_client_order_id = *ExchangeClientOrderId::from(id);
        batch.push_back(cmd);
    }
    const auto request = builder.cancel_batch(batch, k_signed_at);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->method, net::HttpMethod::Delete);
    EXPECT_EQ(request->target, ep::k_batch_orders);
    for (const auto& cmd : batch) {
        EXPECT_NE(request->body.find(cmd.exchange_client_order_id.view()), std::string_view::npos);
    }
    // A DELETE that signs its BODY: the rule is the same one every other verb
    // follows, and the query is empty so no "?" appears.
    EXPECT_EQ(builder.last_sign_message(),
              std::string{k_signed_at_ms} + "DELETE" + std::string{ep::k_batch_orders} +
                  std::string{request->body});

    // Eleven is not ten. WEEX cancels the first ten and says nothing about the
    // rest, so the request is refused here and the caller chunks.
    batch.push_back(batch.front());
    EXPECT_EQ(builder.cancel_batch(batch, k_signed_at).error(), BuildError::NoOrderTarget);
    EXPECT_EQ(builder.cancel_batch({}, k_signed_at).error(), BuildError::NoOrderTarget);
}

TEST(WeexWire, TheRefusalsThisAdapterHasActuallySeenAreClassified) {
    // Both captured from production on 2026-09-13 with no credentials at all —
    // the only WEEX private answers that exist anywhere in this repository.
    const auto unauthorized = parse_rest_error(fixture("rest_error_unauthorized.json"));
    ASSERT_TRUE(unauthorized.has_value());
    EXPECT_EQ(unauthorized->code, k_access_key_invalid);
    // AuthFailure halts the whole venue, which is right for a credential that
    // is wrong for every request this process will make.
    EXPECT_EQ(classify(unauthorized->code), exec::ErrorClass::AuthFailure);

    const auto bad_body = parse_rest_error(fixture("rest_error_bad_body.json"));
    ASSERT_TRUE(bad_body.has_value());
    EXPECT_EQ(bad_body->code, k_invalid_json);
    EXPECT_EQ(classify(bad_body->code), exec::ErrorClass::Terminal);

    // A refusal about ONE contract may not halt the other nine hundred.
    EXPECT_EQ(classify(k_symbol_not_api_tradable), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_key_not_bound_to_symbol), exec::ErrorClass::Terminal);
    // The venue bans for ten seconds after a breach it notices.
    EXPECT_EQ(classify(k_order_rate_limited), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(k_system_error), exec::ErrorClass::Retryable);
    // A cancel for an order the venue no longer has got what it asked for.
    EXPECT_TRUE(is_benign_cancel_failure(k_order_not_found));
    EXPECT_FALSE(is_benign_cancel_failure(k_param_invalid));
}

TEST(WeexWire, AnOrderIdSurvivesAsTextWhicheverJsonTypeItArrivesAs) {
    // WEEX quotes the same id as a STRING on a place answer and as an 18-digit
    // NUMBER on a read — and its own documentation prints 702345678901234600
    // for the order its place example called 702345678901234567, having lost
    // the last digits to a double on the way to the page.
    const auto ack = parse_order_ack(fixture("rest_order_ack.json"));
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->order_id, "702345678901234567");

    static thread_local OrderBuffer storage;
    const auto listed_orders = parse_orders(fixture("rest_open_orders.json"), storage);
    ASSERT_TRUE(listed_orders.has_value());
    ASSERT_FALSE(listed_orders->empty());
    // The same id, quoted as a bare 18-digit number this time, and not one
    // digit short.
    EXPECT_EQ((*listed_orders)[0].order_id, "702345678901234567");
}

TEST(WeexWire, AnAcknowledgementIsReadForItsVerdictAndNotItsStatusLine) {
    // A 200 IS NOT AN ACCEPTANCE on this venue: a refused order arrives inside
    // a successful HTTP response with `success` false and a NAMED errorCode.
    const auto placed = parse_order_ack(fixture("rest_order_ack.json"));
    ASSERT_TRUE(placed.has_value());
    EXPECT_TRUE(placed->success);
    EXPECT_EQ(placed->client_order_id, k_ulid);

    // The cancel answer spells the same field `origClientOrderId`.
    const auto canceled = parse_order_ack(fixture("rest_cancel_ack.json"));
    ASSERT_TRUE(canceled.has_value());
    EXPECT_TRUE(canceled->success);
    EXPECT_EQ(canceled->client_order_id, k_ulid);

    static thread_local OrderAckBuffer storage;
    const auto batch = parse_order_acks(fixture("rest_cancel_batch.json"), storage);
    ASSERT_TRUE(batch.has_value());
    ASSERT_EQ(batch->size(), 2U);
    EXPECT_TRUE((*batch)[0].success);
    // One entry saying no beside one saying yes is the NORMAL shape of a batch,
    // not a failure of the request.
    EXPECT_FALSE((*batch)[1].success);
    EXPECT_TRUE(is_benign_cancel_error((*batch)[1].error_code));
    EXPECT_FALSE(is_benign_cancel_error((*batch)[0].error_code));
}

TEST(WeexWire, LeavesIsASubtractionBecauseTheStatusWordWillNotSayIt) {
    // WEEX's status vocabulary has NO PartiallyFilled. A half-filled order is
    // reported "NEW", and only executedQty against origQty says otherwise — a
    // reader that trusted the word would report a working order as untouched
    // while it filled.
    const auto table = load_table();
    static thread_local OrderBuffer storage;
    const auto orders = parse_orders(fixture("rest_open_orders.json"), storage);
    ASSERT_TRUE(orders.has_value());
    ASSERT_EQ(orders->size(), 2U);
    EXPECT_EQ((*orders)[0].status, WireOrderStatus::New);
    EXPECT_EQ((*orders)[1].status, WireOrderStatus::New);

    const auto untouched = map_open_order((*orders)[0], table);
    ASSERT_TRUE(untouched.has_value());
    EXPECT_EQ(untouched->status, OrderStatus::Accepted);
    EXPECT_EQ(untouched->qty.raw, 200);
    EXPECT_EQ(untouched->leaves.raw, 200);
    EXPECT_EQ(untouched->side, Side::Sell);
    EXPECT_EQ(untouched->price.raw, 695'000);
    EXPECT_EQ(untouched->exchange_order_id.view(), "702345678901234567");
    EXPECT_EQ(untouched->exchange_client_order_id.view(), k_ulid);

    // 0.008 of 0.02 filled, against a 0.0001 lot: 80 of 200, so 120 are left —
    // and the venue still calls it NEW.
    const auto working = map_open_order((*orders)[1], table);
    ASSERT_TRUE(working.has_value());
    EXPECT_EQ(working->status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(working->leaves.raw, 120);

    // And the word does not stop a FULL fill being read as one. WEEX prints no
    // such example, because it would still be calling the order "NEW"; this is
    // that row with its cumulative quantity brought up to its original.
    const auto complete = replacing(
        fixture("rest_open_orders.json"), R"("executedQty":"0.008")", R"("executedQty":"0.02")");
    const auto finished = parse_orders(complete, storage);
    ASSERT_TRUE(finished.has_value());
    const auto done = map_open_order((*finished)[1], table);
    ASSERT_TRUE(done.has_value());
    EXPECT_EQ(done->status, OrderStatus::Filled);
    EXPECT_EQ(done->leaves.raw, 0);
}

TEST(WeexWire, AnOrderStatingNoFilledQuantityLeavesTheRemainderUndef) {
    // UNDEF IS NOT ZERO, and here it is the difference between "nothing is
    // working" and "this adapter could not tell". The edit is one field wide
    // because no capture of a WEEX order exists at all — see fixtures/README.md.
    const auto table = load_table();
    const auto edited = without(fixture("rest_open_orders.json"), R"("executedQty":"0",)");
    static thread_local OrderBuffer storage;
    const auto orders = parse_orders(edited, storage);
    ASSERT_TRUE(orders.has_value());
    ASSERT_FALSE(orders->empty());

    const auto report = map_open_order((*orders)[0], table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->qty.raw, 200);
    EXPECT_TRUE(report->leaves.is_undef());
    // The order is still reported as WORKING: the row came back from the
    // open-order list, and dropping it would read downstream as an order the
    // venue no longer has.
    EXPECT_EQ(report->status, OrderStatus::Accepted);
}

TEST(WeexWire, AWordThisAdapterDoesNotKnowFallsBackToTheQuantities) {
    // WEEX has added status words before and will again; a row carrying one is
    // read for what its numbers say rather than dropped, because dropping it
    // reads downstream as an order the venue no longer has.
    const auto table = load_table();
    const auto edited =
        replacing(fixture("rest_open_orders.json"), R"("status":"NEW")", R"("status":"WHATEVER")");
    static thread_local OrderBuffer storage;
    const auto orders = parse_orders(edited, storage);
    ASSERT_TRUE(orders.has_value());
    ASSERT_FALSE(orders->empty());
    EXPECT_EQ((*orders)[0].status, WireOrderStatus::Unknown);

    const auto report = map_open_order((*orders)[0], table);
    ASSERT_TRUE(report.has_value());
    // Nothing has filled, so it is working — the same reading the known word
    // would have produced.
    EXPECT_EQ(report->status, OrderStatus::Accepted);
    EXPECT_EQ(report->leaves.raw, 200);
}

TEST(WeexWire, AFillCarriesItsFeeItsLiquidityAndTheOrderItBelongsTo) {
    const auto table = load_table();
    static thread_local FillBuffer storage;
    const auto fills = parse_fills(fixture("rest_user_trades.json"), storage);
    ASSERT_TRUE(fills.has_value());
    ASSERT_EQ(fills->size(), 1U);

    const auto report = map_fill((*fills)[0], table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->instrument, listed(table, "BTCUSDT").id);
    EXPECT_EQ(report->side, Side::Buy);
    EXPECT_EQ(report->price.raw, 690'000);
    EXPECT_EQ(report->qty.raw, 100); // 0.01 against a 0.0001 lot
    EXPECT_EQ(report->fee.raw, 138'000'000);
    EXPECT_EQ(report->liquidity, Liquidity::Taker);
    EXPECT_EQ(report->exec_id.view(), "801234567890123456");
    EXPECT_EQ(report->exchange_order_id.view(), "702345678901234567");
    // NO CLIENT ORDER ID RIDES A FILL on this venue, and an empty id here is
    // the venue declining to say rather than an id this adapter guessed.
    EXPECT_TRUE(report->exchange_client_order_id.empty());
    EXPECT_EQ(report->ts_venue.ns, 1'764'505'701'456ULL * 1'000'000ULL);
}

TEST(WeexWire, AFillStatingNoCommissionLeavesTheFeeUndef) {
    const auto table = load_table();
    const auto edited = without(fixture("rest_user_trades.json"), R"("commission":"0.138",)");
    static thread_local FillBuffer storage;
    const auto fills = parse_fills(edited, storage);
    ASSERT_TRUE(fills.has_value());
    const auto report = map_fill((*fills)[0], table);
    ASSERT_TRUE(report.has_value());
    // A fee of zero is a claim that the fill was free.
    EXPECT_TRUE(report->fee.is_undef());
    EXPECT_EQ(report->qty.raw, 100);
}

TEST(WeexWire, PositionsAreSignedByTheirSideAndNettedPerInstrument) {
    // `size` is UNSIGNED here and `side` is the only thing that says which way
    // the account faces. And under `separatedMode` SEPARATED one contract
    // carries a row per direction: everything downstream keys a position on the
    // instrument alone, so two rows published raw become whichever arrived
    // last — a hedged book reported as one naked leg.
    const auto table = load_table();
    static thread_local PositionBuffer storage;
    const auto rows = parse_positions(fixture("rest_positions.json"), storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 3U);

    const auto reports = map_positions(*rows, table);
    ASSERT_EQ(reports.size(), 2U);

    const auto btc = listed(table, "BTCUSDT").id;
    const auto doge = listed(table, "DOGEUSDT").id;
    for (const auto& report : reports) {
        if (report.instrument == btc) {
            // 0.02 long and 0.005 short against a 0.0001 lot: 200 - 50.
            EXPECT_EQ(report.net_qty.raw, 150);
            // The later of the two rows merged.
            EXPECT_EQ(report.ts_venue.ns, 1'764'588'999'999ULL * 1'000'000ULL);
        } else {
            EXPECT_EQ(report.instrument, doge);
            EXPECT_EQ(report.net_qty.raw, -15); // 1500 short against a lot of 100
        }
        // WEEX STATES NO ENTRY PRICE. openValue over size is a quotient, not a
        // price the venue quoted, and it need not land on the contract's tick.
        EXPECT_TRUE(report.avg_entry.is_undef());
    }
}

TEST(WeexWire, ABalanceStatesEachColumnOrLeavesItUndef) {
    static thread_local BalanceBuffer storage;
    const auto coins = parse_balances(fixture("rest_balance.json"), storage);
    ASSERT_TRUE(coins.has_value());
    ASSERT_EQ(coins->size(), 1U);

    const auto report = map_balance((*coins)[0], k_ingest);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->asset.view(), "USDT");
    EXPECT_EQ(report->total.raw, 5'696'492'888'230);
    EXPECT_EQ(report->available.raw, 5'413'068'773'690);
    EXPECT_EQ(report->unrealized.raw, -34'553'000'000);
    EXPECT_EQ(report->ts_venue, k_ingest);

    // A column the venue did not state is UNDEF, never zero: an available
    // balance of zero invites a caller to conclude nothing may be committed.
    const auto edited =
        without(fixture("rest_balance.json"), R"("availableBalance":"5413.06877369",)");
    const auto thin = parse_balances(edited, storage);
    ASSERT_TRUE(thin.has_value());
    const auto partial = map_balance((*thin)[0], k_ingest);
    ASSERT_TRUE(partial.has_value());
    EXPECT_TRUE(partial->available.is_undef());
    EXPECT_EQ(partial->total.raw, 5'696'492'888'230);
}

TEST(WeexWire, OnlyFundingRowsAreBookedAndTheSignIsTheAccountsOwn) {
    // A payment and a receipt differ only by sign, and WEEX already states the
    // one the ledger wants: `income` is a COLLATERAL CHANGE, so a settlement
    // received is positive and one paid away negative. Flipping it here would
    // make every funding series run backwards.
    const auto table = load_table();
    static thread_local BillBuffer storage;
    const auto bills = parse_bills(fixture("rest_income.json"), storage);
    ASSERT_TRUE(bills.has_value());
    ASSERT_EQ(bills->size(), 3U);

    const auto received = map_funding_charge((*bills)[0], table, k_ingest);
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->amount.raw, 82'666'460);
    EXPECT_EQ(received->kind, LedgerKind::Funding);
    EXPECT_EQ(received->reference.view(), "686960019383517338");
    EXPECT_EQ(received->settled_at.ns, 1'763'784'031'721ULL * 1'000'000ULL);
    EXPECT_EQ(received->instrument, listed(table, "BTCUSDT").id);

    const auto paid = map_funding_charge((*bills)[1], table, k_ingest);
    ASSERT_TRUE(paid.has_value());
    EXPECT_EQ(paid->amount.raw, -119'302'440);

    // The bill list answers every deposit and transfer the account ever made,
    // and booking one as funding is money invented.
    EXPECT_FALSE(is_funding_bill((*bills)[2]));
    EXPECT_FALSE(map_funding_charge((*bills)[2], table, k_ingest).has_value());
}

TEST(WeexWire, AnUnreadableAmountIsNotAChargeOfZero) {
    const auto table = load_table();
    const auto edited = without(fixture("rest_income.json"), R"("income":"0.08266646",)");
    static thread_local BillBuffer storage;
    const auto bills = parse_bills(edited, storage);
    ASSERT_TRUE(bills.has_value());
    // Absent, not zero: a settlement booked at nothing is a settlement lost.
    EXPECT_FALSE(map_funding_charge((*bills)[0], table, k_ingest).has_value());
}

TEST(WeexWire, APrivateBodyThatIsAnErrorIsNeverReadAsAnEmptyAccount) {
    // The refusal shapes are the ones this adapter has actually seen, and each
    // of them has to fail the LIST readers rather than look like a flat book.
    static thread_local PositionBuffer positions;
    EXPECT_FALSE(parse_positions(fixture("rest_error_unauthorized.json"), positions).has_value());
    static thread_local BalanceBuffer balances;
    EXPECT_FALSE(parse_balances(fixture("rest_error_unauthorized.json"), balances).has_value());
    static thread_local OrderBuffer orders;
    EXPECT_FALSE(parse_orders(fixture("rest_error_unauthorized.json"), orders).has_value());
    static thread_local FillBuffer fills;
    EXPECT_FALSE(parse_fills(fixture("rest_error_bad_body.json"), fills).has_value());
    static thread_local BillBuffer bills;
    EXPECT_FALSE(parse_bills(fixture("rest_error_bad_body.json"), bills).has_value());
}

TEST(WeexWire, TheServerClockIsReadBecauseASignatureExpires) {
    // WEEX refuses a signature stamped more than 30 s from its own clock, and a
    // drifting laptop wears the face of a revoked key.
    const auto now = parse_server_time(R"({"serverTime":1789325256837})");
    ASSERT_TRUE(now.has_value());
    EXPECT_EQ(*now, 1'789'325'256'837ULL);
    // A body that does not say reads as zero, and zero means "no correction",
    // never "the epoch".
    EXPECT_EQ(parse_server_time(R"({"noSuchField":1})").value_or(1), 0ULL);
}

} // namespace
} // namespace raikiri::venues::weex
