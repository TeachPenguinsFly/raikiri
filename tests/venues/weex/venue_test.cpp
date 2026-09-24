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

// the fixture dir is located from this file; held because parsed views point into it
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

std::string replacing(std::string_view body, std::string_view from, std::string_view to) {
    std::string out{body};
    const auto at = out.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    out.replace(at, from.size(), to);
    return out;
}

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
    const auto& caps = exec::caps_of(VenueId::Weex);
    EXPECT_TRUE(caps.markets.perp);
    EXPECT_FALSE(caps.markets.spot);
    EXPECT_FALSE(caps.markets.future);
    EXPECT_FALSE(caps.markets.option);
    EXPECT_FALSE(caps.ws_order_entry);
    EXPECT_FALSE(caps.fix_order_entry);
    EXPECT_TRUE(caps.cancel_by_client_order_id);
    EXPECT_FALSE(caps.batch_cancel);
    // weex has no modify for a resting order
    EXPECT_FALSE(caps.amend_order);
    EXPECT_FALSE(caps.native_post_only);
    EXPECT_TRUE(caps.position_pull.has(MarketType::Perp));
    EXPECT_TRUE(caps.order_pull.has(MarketType::Perp));
    EXPECT_TRUE(caps.fill_pull.has(MarketType::Perp));
    EXPECT_TRUE(caps.balance_pull.has(MarketType::Perp));
    EXPECT_TRUE(caps.funding_charge.has(MarketType::Perp));
    EXPECT_FALSE(caps.position_pull.has(MarketType::Spot));
    EXPECT_FALSE(caps.order_pull.has(MarketType::Spot));
    EXPECT_FALSE(caps.balance_pull.has(MarketType::Spot));
    EXPECT_EQ(caps.user_stream, exec::UserStreamModel::None);
    EXPECT_TRUE(caps.ticker_stats);

    EXPECT_TRUE(caps.needs_passphrase);
    EXPECT_EQ(caps.max_client_order_id_len, k_max_client_order_id);
    EXPECT_GE(caps.max_client_order_id_len, exec::k_min_client_order_id_len);
    // weex states 300 orders a minute per account, no per-second figure
    EXPECT_EQ(caps.headline_order_rate_per_s, 0U);
}

TEST(WeexVenue, TwoOfThreeCredentialsIsRefusedAtStartupRatherThanPerOrder) {
    // without the passphrase weex answers -1049
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

    EXPECT_TRUE(exec::make_venue({.venue = VenueId::Weex, .market = MarketType::Perp}, sink, clock)
                    .has_value());
}

TEST(WeexVenue, WithNoCredentialEveryCommandIsRefusedLocallyRatherThanLeftOpen) {
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
    EXPECT_EQ(sink.events[0].leaves_qty.raw, 0);

    cmd.kind = CmdKind::Cancel;
    adapter->execution->cancel(cmd);
    ASSERT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(sink.events[1].type, OrderEventType::CancelRejected);
    EXPECT_EQ(sink.events[1].leaves_qty.raw, 7);

    adapter->execution->amend(cmd);
    ASSERT_EQ(sink.events.size(), 3U);
    EXPECT_EQ(sink.events[2].type, OrderEventType::ModifyRejected);

    EXPECT_FALSE(adapter->execution->open_order_reports(MarketType::Perp).has_value());
    EXPECT_FALSE(adapter->execution->position_reports(MarketType::Perp).has_value());
    EXPECT_FALSE(adapter->execution->fill_reports(MarketType::Perp, UnixNanos{}).has_value());
    EXPECT_FALSE(adapter->execution->balance_reports(MarketType::Perp).has_value());
}

TEST(WeexVenue, MarketsWithNoCatalogueLoaderAreRefusedRatherThanReturnedEmpty) {
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
        EXPECT_FALSE(event->symbol.empty()) << file;
    }
}

TEST(WeexWire, TheSnapshotIsADIFFERENTKINDFromTheDeltaOnTheSameChannel) {
    // only "e" separates snapshot and delta
    const auto snapshot = parse_event(fixture("ws_depth_snapshot.json"));
    const auto delta = parse_event(fixture("ws_depth_update.json"));
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_TRUE(delta.has_value());
    EXPECT_NE(snapshot->kind, delta->kind);
    EXPECT_NE(parse_event(fixture("ws_trade_snapshot.json"))->kind,
              parse_event(fixture("ws_trade_buy.json"))->kind);
}

TEST(WeexWire, AnAcknowledgementAndARefusalAreOneBooleanApart) {
    const auto ack = parse_event(fixture("ws_subscribe_ack.json"));
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->kind, EventKind::SubscribeAck);

    const auto refused = fixture("ws_subscribe_refused.json");
    const auto bad = parse_event(refused);
    ASSERT_TRUE(bad.has_value());
    EXPECT_EQ(bad->kind, EventKind::Error);

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
    // an unrouted path has no code; the caller's http check catches it
    const auto missing = parse_rest_error(fixture("rest_error_not_found.json"));
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(missing->code, k_ok);
    EXPECT_EQ(parse_rest_error(fixture("rest_exchange_info.json"))->code, k_ok);
    EXPECT_EQ(parse_rest_error(fixture("rest_premium_index.json"))->code, k_ok);
}

TEST(WeexBook, TheEventAndTheDispositionMustAgree) {
    static thread_local LevelBuffer storage;
    const auto snapshot = parse_depth(fixture("ws_depth_snapshot.json"), storage);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_TRUE(snapshot->is_snapshot);
    EXPECT_EQ(snapshot->tier, 15U);

    const auto delta = parse_depth(fixture("ws_depth_update.json"), storage);
    ASSERT_TRUE(delta.has_value());
    EXPECT_FALSE(delta->is_snapshot);

    // edited: no live frame disagrees with itself
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
    // U equals the previous frame's u
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
    // tick 0.1, lot 0.0001
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
    // the next frame sends 79346.5 at "0"
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
    EXPECT_EQ(after.size(), 15U);
    EXPECT_EQ(book.asks().size(), 15U);
}

TEST(WeexBook, TheTwoHundredLevelChannelPublishesTheTopHundred) {
    const auto table = load_table();
    const auto& btc = listed(table, "BTCUSDT");
    DepthBook book{btc.id, btc.tick_size, btc.lot_size};

    static thread_local LevelBuffer storage;
    const auto seed = parse_depth(fixture("ws_depth200_snapshot.json"), storage);
    ASSERT_TRUE(seed.has_value());
    EXPECT_EQ(seed->tier, 200U);
    EXPECT_EQ(seed->bids.size(), 200U);
    EXPECT_EQ(seed->asks.size(), 200U);
    ASSERT_EQ(book.apply(*seed), BookResult::Applied);
    ASSERT_EQ(book.apply(*parse_depth(fixture("ws_depth200_update.json"), storage)),
              BookResult::Applied);
    EXPECT_EQ(book.dropped_levels(), 0U);
    EXPECT_GE(book.bids().size(), k_book_levels);
    EXPECT_GE(book.asks().size(), k_book_levels);

    const auto snap = book.snapshot(UnixNanos{}, UnixNanos{});
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->bid_count, k_book_levels);
    EXPECT_EQ(snap->ask_count, k_book_levels);
    EXPECT_EQ(snap->bids[0].price.raw, book.bids().front().price.raw);
    EXPECT_EQ(snap->bids[k_book_levels - 1].price.raw, book.bids()[k_book_levels - 1].price.raw);
    EXPECT_GT(snap->asks[k_book_levels - 1].price.raw, snap->asks[0].price.raw);
}

TEST(WeexBook, TheTopOfBookIsTheOnlyQuoteThisVenueHas) {
    const auto table = load_table();
    const auto& btc = listed(table, "BTCUSDT");
    DepthBook book{btc.id, btc.tick_size, btc.lot_size};
    static thread_local LevelBuffer storage;

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

TEST(WeexBook, AGappedBookIsReseededOncePerGap) {
    const auto table = load_table();
    const auto& btc = listed(table, "BTCUSDT");
    DepthBook book{btc.id, btc.tick_size, btc.lot_size};
    static thread_local LevelBuffer storage;
    constexpr std::uint64_t s = 1'000'000'000ULL;
    BookWatch watch{.instrument = btc.id};

    ASSERT_EQ(book.apply(*parse_depth(fixture("ws_depth_snapshot.json"), storage)),
              BookResult::Applied);
    watch.seen_ns = 1'000 * s;
    EXPECT_EQ(due_for_reseed(watch, 1'010 * s), Reseed::None);
    watch.gapped = book.apply(*parse_depth(fixture("ws_depth_update_next.json"), storage)) ==
                   BookResult::Gap;
    ASSERT_TRUE(watch.gapped);
    EXPECT_EQ(due_for_reseed(watch, 1'011 * s), Reseed::Gap);
    EXPECT_EQ(due_for_reseed(watch, 1'012 * s), Reseed::None) << "one resubscribe, not one per frame";
    watch.gapped = true;
    EXPECT_EQ(due_for_reseed(watch, 1'100 * s), Reseed::None)
        << "the snapshot gets its silence to arrive";
    EXPECT_EQ(due_for_reseed(watch, 1'011 * s + k_book_silence_ns), Reseed::Gap);

    Protocol protocol;
    const std::array<std::string_view, 1> one{"BTCUSDT@depth200"};
    const auto off = protocol.unsubscribe_frame(one);
    ASSERT_TRUE(off.has_value());
    EXPECT_EQ(off->text, R"({"method":"UNSUBSCRIBE","params":["BTCUSDT@depth200"],"id":1})");
}

TEST(WeexBook, AQuietBookBacksOffWhileItsSnapshotIsUnchanged) {
    const auto table = load_table();
    const auto& btc = listed(table, "BTCUSDT");
    DepthBook book{btc.id, btc.tick_size, btc.lot_size};
    static thread_local LevelBuffer storage;
    ASSERT_EQ(book.apply(*parse_depth(fixture("ws_depth_snapshot.json"), storage)),
              BookResult::Applied);
    constexpr std::uint64_t s = 1'000'000'000ULL;
    constexpr std::uint64_t min = 60 * s;
    const auto still = book_digest(book);

    BookWatch watch{.instrument = btc.id, .seen_ns = 2'000 * s};
    std::uint64_t now = 2'000 * s + k_book_silence_ns - 1;
    EXPECT_EQ(due_for_reseed(watch, now), Reseed::None);
    now += 1;
    EXPECT_EQ(due_for_reseed(watch, now), Reseed::Silent);
    std::vector<std::uint64_t> waits;
    for (int i = 0; i < 6; ++i) {
        watch.digest = still;
        watch.seen_ns = now + s;
        EXPECT_FALSE(on_reseed_snapshot(watch, still));
        const auto asked = now;
        while (due_for_reseed(watch, now) == Reseed::None) {
            now += s;
        }
        waits.push_back((now - asked) / min);
        EXPECT_EQ(watch.asked_ns, now);
    }
    EXPECT_EQ(waits, (std::vector<std::uint64_t>{10, 20, 40, 60, 60, 60}));

    watch.digest = still;
    EXPECT_TRUE(on_reseed_snapshot(watch, still ^ 2U)) << "the venue moved while we heard nothing";
    EXPECT_EQ(watch.silence_ns, k_book_silence_ns);
    EXPECT_EQ(due_for_reseed(watch, now + k_book_silence_ns), Reseed::Silent)
        << "a stream that stopped is a new episode, logged again";

    on_delta(watch, now + 2 * k_book_silence_ns);
    EXPECT_EQ(watch.silence_ns, k_book_silence_ns);
    EXPECT_EQ(due_for_reseed(watch, now + 3 * k_book_silence_ns), Reseed::Silent);
    watch.gapped = true;
    EXPECT_EQ(due_for_reseed(watch, now + 4 * k_book_silence_ns), Reseed::Gap)
        << "a gap never waits out the backoff";

    Protocol protocol;
    const std::array<std::string_view, 1> one{"BTCUSDT@depth200"};
    const auto off = protocol.unsubscribe_frame(one);
    ASSERT_TRUE(off.has_value());
    EXPECT_EQ(off->text, R"({"method":"UNSUBSCRIBE","params":["BTCUSDT@depth200"],"id":1})");
}

TEST(WeexBook, ABrokenChainInvalidatesRatherThanServingAStaleBook) {
    const auto table = load_table();
    const auto& btc = listed(table, "BTCUSDT");
    DepthBook book{btc.id, btc.tick_size, btc.lot_size};
    static thread_local LevelBuffer storage;

    ASSERT_EQ(book.apply(*parse_depth(fixture("ws_depth_snapshot.json"), storage)),
              BookResult::Applied);
    EXPECT_EQ(book.apply(*parse_depth(fixture("ws_depth_update_next.json"), storage)),
              BookResult::Gap);
    EXPECT_EQ(book.gaps(), 1U);
    EXPECT_FALSE(book.live());
    EXPECT_FALSE(book.snapshot(k_ingest, k_ingest).has_value());
    EXPECT_FALSE(book.touch(k_ingest, k_ingest).has_value());

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
    // the same book read with pricePrecision 0 instead of 1
    DepthBook book{btc.id, FixedE9::from_raw(1'000'000'000), btc.lot_size};
    static thread_local LevelBuffer storage;

    EXPECT_EQ(book.apply(*parse_depth(fixture("ws_depth_snapshot.json"), storage)),
              BookResult::Unseeded);
    // 79348.0 is the one whole-unit bid; the one-sided remainder publishes nothing
    EXPECT_EQ(book.dropped_levels(), 29U);
    EXPECT_FALSE(book.live());
}

TEST(WeexMap, TheMakerFlagIsTheAggressorInverted) {
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
    static thread_local PrintBuffer storage;
    const auto prints = parse_trades(fixture("ws_trade_buy.json"), storage);
    ASSERT_TRUE(prints.has_value());
    EXPECT_EQ((*prints)[0].trade_id, "b6e4d7bc-dff4-4f21-acc8-2976c16746d2");
    EXPECT_EQ((*prints)[0].trade_ms, 1'788'774'085'716ULL);
}

TEST(WeexMap, EachPrintCarriesItsOwnTimeAndNotTheFramesSendTime) {
    // two prints 406 ms apart in one message
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
    // DOGEUSDT's quantityPrecision is -2, so its size steps in hundreds
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
    // the quote/base ratio is the weighted average price, which pins the columns and base-amount sizes
    EXPECT_EQ(stats->volume_24h_quote.raw * (k_e9 / k_e6) / stats->volume_24h_base.raw, 79'663);
    EXPECT_EQ(stats->ts_event.ns, 1'788'773'400'000'000'000ULL);
}

TEST(WeexMap, AColumnTheTickerDidNotStateIsUndefAndNotZero) {
    // edited: every live frame carries both columns
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

TEST(WeexMap, FundingAtZeroIsNotTheSameAsQuotingNoFunding) {
    const auto table = load_table();
    static thread_local PremiumBuffer storage;
    const auto rows = parse_premium_index(fixture("rest_premium_index.json"), storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);

    // USDCUSDT funds at exactly zero
    const auto zero = map_funding((*rows)[1], table, k_ingest);
    ASSERT_TRUE(zero.has_value());
    EXPECT_EQ((*rows)[1].symbol, "USDCUSDT");
    EXPECT_EQ(zero->rate.raw, 0);
    EXPECT_FALSE(zero->rate.is_undef());

    // edited: no live row omits the rate
    auto silent = (*rows)[0];
    silent.funding_rate = {};
    EXPECT_FALSE(map_funding(silent, table, k_ingest).has_value());
}

TEST(WeexMap, ARateFinerThanFixedPointIsTruncatedRatherThanRefused) {
    // GER40USDT pads the rate to eleven digits
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

    // THEUSDT's index is off its 1e-5 tick; its mark divides
    const auto& the = listed(table, "THEUSDT");
    EXPECT_EQ(the.tick_size.raw, 10'000);
    const auto the_funding = map_funding((*rows)[3], table, k_ingest);
    ASSERT_TRUE(the_funding.has_value());
    EXPECT_EQ(the_funding->mark_price.raw, 7'323); // "0.07323" divides
    EXPECT_EQ((*rows)[3].symbol, "THEUSDT");

    // edited: every live row states a mark
    auto markless = (*rows)[0];
    markless.mark_price = "79421.45"; // half a tick, which WEEX cannot quote
    const auto refused = map_funding(markless, table, k_ingest);
    ASSERT_TRUE(refused.has_value());
    EXPECT_TRUE(refused->mark_price.is_undef());
    EXPECT_NE(refused->mark_price.raw, 0);
}

TEST(WeexMap, TheFundingIntervalComesFromTheCataloguesSettlementTimetable) {
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

    // 86400 / the delivery timetable's length
    const std::array<Expected, 8> table{{
        {"BTCUSDT", true, 100'000'000, 100'000, 28'800},
        {"DOGEUSDT", true, 10'000, 100'000'000'000, 28'800},
        {"EURUSDT", true, 100'000, 1'000'000'000, 14'400},
        {"ONGUSDT", true, 10'000, 10'000'000'000, 3'600},
        {"THEUSDT", true, 10'000, 1'000'000'000, 14'400},
        {"USDCUSDT", true, 1'000, 1'000'000'000, 28'800},
        {"GER40USDT", true, 10'000'000, 100'000, 28'800},
        // pricePrecision 10 is a step of 1e-10, finer than FixedE9
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
    // bare array of names, no envelope
    static thread_local TradableBuffer storage;
    const auto names = parse_api_trading_symbols(fixture("rest_api_trading_symbols.json"), storage);
    ASSERT_TRUE(names.has_value());
    ASSERT_EQ(names->size(), 5U);
    EXPECT_EQ((*names)[0], "1000BONKUSDT");
    EXPECT_EQ((*names)[2], "BTCUSDT");
    EXPECT_EQ((*names)[4], "XRPUSDT");
}

TEST(WeexCatalogue, AnEmptyTradableSetRefusesNothing) {
    // empty means unknown
    static thread_local TradableBuffer storage;
    const auto empty = parse_api_trading_symbols("[]", storage);
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ(empty->size(), 0U);
}

TEST(WeexCatalogue, ASizeStepIsReadAsADecimalAndNeverAsAnExponent) {
    // contractVal is the size step; quantityPrecision can be negative
    static thread_local InstrumentBuffer storage;
    const auto rows = parse_exchange_info(fixture("rest_exchange_info.json"), storage);
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ((*rows)[1].contract_val, "100");    // DOGEUSDT, precision -2
    EXPECT_EQ((*rows)[0].contract_val, "0.0001"); // BTCUSDT, precision 4
    // written as an int on one row and a float on the next
    EXPECT_EQ((*rows)[3].contract_val, "10"); // ONGUSDT, precision -1
}

TEST(WeexCatalogue, TradifiPerpetualsAreAdmittedAndPricedLikeAnyOther) {
    // TRADIFI_PERPETUAL rows settle and fund in USDT like the rest
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
    // weex states no min notional; minOrderSize is a quantity
    static thread_local InstrumentBuffer storage;
    const auto rows = parse_exchange_info(fixture("rest_exchange_info.json"), storage);
    ASSERT_TRUE(rows.has_value());
    const auto btc = map_instrument((*rows)[0], MarketType::Perp);
    ASSERT_TRUE(btc.has_value());
    const auto& base = base_of(*btc);
    EXPECT_TRUE(base.min_notional.is_undef());
    EXPECT_TRUE(base.maker_fee.is_undef());
    EXPECT_TRUE(base.taker_fee.is_undef());
    EXPECT_TRUE(validate(base));
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

TEST(WeexWs, BboAndDepthNameOneChannel) {
    // weex has no quote channel, so Bbo and Depth share one
    std::array<char, 64> quote{};
    std::array<char, 64> ladder{};
    const auto bbo = channel_name(exec::SubKind::Bbo, "BTCUSDT", quote);
    const auto depth = channel_name(exec::SubKind::Depth, "BTCUSDT", ladder);
    EXPECT_EQ(bbo, "BTCUSDT@depth200");
    EXPECT_EQ(bbo, depth);
}

TEST(WeexWs, TheSymbolGoesOnTheWireAsTheCatalogueSpelledIt) {
    // channels are case-insensitive, so the symbol travels unchanged
    std::array<char, 64> buf{};
    EXPECT_EQ(channel_name(exec::SubKind::Trades, "DOGEUSDT", buf), "DOGEUSDT@trade");
    EXPECT_EQ(channel_name(exec::SubKind::MarkPrice, "BTCUSDT", buf), "BTCUSDT@ticker");
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

    std::vector<std::string_view> many(30, "BTCUSDT@trade");
    const auto batched = protocol.subscribe_frame(many);
    ASSERT_TRUE(batched.has_value());
    EXPECT_EQ(batched->consumed, 30U);
}

TEST(WeexWs, AnEmptyChannelIsRefusedRatherThanTakingTheFrameDownWithIt) {
    // one bad member refuses the whole params array
    Protocol protocol;
    const std::array<std::string_view, 2> poisoned{"BTCUSDT@depth15", {}};
    const auto frame = protocol.subscribe_frame(poisoned);
    ASSERT_FALSE(frame.has_value());
    EXPECT_EQ(frame.error(), WsBuildError::UnsupportedKind);
    EXPECT_FALSE(protocol.subscribe_frame({}).has_value());
}

TEST(WeexWs, EveryAddressIsSpelledOnceAndTheBuilderUsesIt) {
    EXPECT_EQ(stream_host(), ep::k_stream_host);
    EXPECT_EQ(stream_path(), ep::k_ws_public);
    EXPECT_EQ(rest_host(), ep::k_rest_host);

    RequestBuilder builder;
    EXPECT_EQ(builder.instruments().target, ep::k_exchange_info);
    EXPECT_EQ(builder.premium_index().target, ep::k_premium_index);
}

// test credentials; these tests pin the signed message
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
    // a GET signs its query, and a POST signs no query it does not send
    TestSigner credentials;
    RequestBuilder builder{credentials.signer, k_passphrase};

    const auto get = builder.open_orders(MarketType::Perp, k_signed_at);
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ(builder.last_sign_message(),
              std::string{k_signed_at_ms} + "GET" + std::string{ep::k_open_orders} +
                  "?limit=100&page=0");
    // "?" appears once, in the message and the target
    EXPECT_EQ(get->target, std::string{ep::k_open_orders} + "?limit=100&page=0");

    const auto table = load_table();
    const auto post = builder.create_order(limit_order(table), table, k_signed_at);
    ASSERT_TRUE(post.has_value());
    const auto message = std::string{builder.last_sign_message()};
    EXPECT_EQ(message,
              std::string{k_signed_at_ms} + "POST" + std::string{ep::k_order} +
                  std::string{post->body});
    // an empty query takes no "?"
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
    // base64: 32 digest bytes become 44 characters
    EXPECT_EQ(digest.size(), net::k_base64_bytes);
    EXPECT_EQ(digest.back(), '=');

    EXPECT_EQ(header_of(*request, "ACCESS-KEY"), "weex-test-key");
    EXPECT_EQ(header_of(*request, "ACCESS-TIMESTAMP"), k_signed_at_ms);
    EXPECT_EQ(header_of(*request, "ACCESS-PASSPHRASE"), k_passphrase);
    // weex answers -1045 to any other content type
    EXPECT_EQ(request->content_type, "application/json");
}

TEST(WeexSign, TheDigestFollowsTheREQUESTAndNotTheOtherWayRound) {
    TestSigner credentials;
    RequestBuilder builder{credentials.signer, k_passphrase};

    ASSERT_TRUE(builder.positions(MarketType::Perp, k_signed_at).has_value());
    const std::string positions{builder.last_sign_message()};
    ASSERT_TRUE(builder.balances(k_signed_at).has_value());
    const std::string balances{builder.last_sign_message()};
    EXPECT_NE(positions, balances);

    ASSERT_TRUE(builder.positions(MarketType::Perp, k_signed_at).has_value());
    EXPECT_EQ(builder.last_sign_message(), positions);
}

TEST(WeexSign, HalfACredentialIsNoCredential) {
    TestSigner credentials;
    RequestBuilder without_passphrase{credentials.signer, {}};
    const auto refused = without_passphrase.positions(MarketType::Perp, k_signed_at);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), BuildError::NotSigned);

    RequestBuilder unsigned_builder;
    EXPECT_FALSE(unsigned_builder.balances(k_signed_at).has_value());
    EXPECT_EQ(unsigned_builder.instruments().target, ep::k_exchange_info);
}

TEST(WeexSign, EveryPrivatePathAddressesTheContractHost) {
    // spot is api-spot.weex.com under /api/v3; this host serves /capi/v3
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

    auto no_qty = limit_order(table);
    no_qty.qty = QtyLots::undef();
    ASSERT_FALSE(builder.create_order(no_qty, table, k_signed_at).has_value());
    EXPECT_EQ(builder.create_order(no_qty, table, k_signed_at).error(), BuildError::BadValue);

    auto no_price = limit_order(table);
    no_price.price = PriceTicks::undef();
    EXPECT_EQ(builder.create_order(no_price, table, k_signed_at).error(), BuildError::BadValue);

    auto no_id = limit_order(table);
    no_id.exchange_client_order_id = {};
    EXPECT_EQ(builder.create_order(no_id, table, k_signed_at).error(), BuildError::NoOrderTarget);

    EXPECT_EQ(builder.positions(MarketType::Spot, k_signed_at).error(),
              BuildError::UnsupportedMarket);
}

TEST(WeexOrders, AReduceOnlyOrderFacesThePositionItCloses) {
    // an opening order faces its side; a reduce-only order faces the opposite
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
    // reduceOnly still travels; positionSide alone does not stop an opposite open
    EXPECT_NE(side_in_body(Side::Buy, OrderFlags::ReduceOnly).find(R"("reduceOnly":true)"),
              std::string::npos);
}

TEST(WeexOrders, PostOnlyIsATimeInForceAndAMarketOrderCarriesNone) {
    // post-only lives in timeInForce
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

    // timeInForce and price are for limit orders only
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
    // a DELETE signs its body; empty query, no "?"
    EXPECT_EQ(builder.last_sign_message(),
              std::string{k_signed_at_ms} + "DELETE" + std::string{ep::k_batch_orders} +
                  std::string{request->body});

    // weex would cancel the first ten and ignore the rest
    batch.push_back(batch.front());
    EXPECT_EQ(builder.cancel_batch(batch, k_signed_at).error(), BuildError::NoOrderTarget);
    EXPECT_EQ(builder.cancel_batch({}, k_signed_at).error(), BuildError::NoOrderTarget);
}

TEST(WeexWire, TheRefusalsThisAdapterHasActuallySeenAreClassified) {
    const auto unauthorized = parse_rest_error(fixture("rest_error_unauthorized.json"));
    ASSERT_TRUE(unauthorized.has_value());
    EXPECT_EQ(unauthorized->code, k_access_key_invalid);
    EXPECT_EQ(classify(unauthorized->code), exec::ErrorClass::AuthFailure);

    const auto bad_body = parse_rest_error(fixture("rest_error_bad_body.json"));
    ASSERT_TRUE(bad_body.has_value());
    EXPECT_EQ(bad_body->code, k_invalid_json);
    EXPECT_EQ(classify(bad_body->code), exec::ErrorClass::Terminal);

    EXPECT_EQ(classify(k_symbol_not_api_tradable), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_key_not_bound_to_symbol), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_order_rate_limited), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(k_system_error), exec::ErrorClass::Retryable);
    EXPECT_TRUE(is_benign_cancel_failure(k_order_not_found));
    EXPECT_FALSE(is_benign_cancel_failure(k_param_invalid));
}

TEST(WeexWire, AnOrderIdSurvivesAsTextWhicheverJsonTypeItArrivesAs) {
    // ids arrive as a string on place and an 18-digit number on reads
    const auto ack = parse_order_ack(fixture("rest_order_ack.json"));
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->order_id, "702345678901234567");

    static thread_local OrderBuffer storage;
    const auto listed_orders = parse_orders(fixture("rest_open_orders.json"), storage);
    ASSERT_TRUE(listed_orders.has_value());
    ASSERT_FALSE(listed_orders->empty());
    EXPECT_EQ((*listed_orders)[0].order_id, "702345678901234567");
}

TEST(WeexWire, AnAcknowledgementIsReadForItsVerdictAndNotItsStatusLine) {
    // a refused order arrives in a 200 with success false
    const auto placed = parse_order_ack(fixture("rest_order_ack.json"));
    ASSERT_TRUE(placed.has_value());
    EXPECT_TRUE(placed->success);
    EXPECT_EQ(placed->client_order_id, k_ulid);

    // the cancel answer says origClientOrderId
    const auto canceled = parse_order_ack(fixture("rest_cancel_ack.json"));
    ASSERT_TRUE(canceled.has_value());
    EXPECT_TRUE(canceled->success);
    EXPECT_EQ(canceled->client_order_id, k_ulid);

    static thread_local OrderAckBuffer storage;
    const auto batch = parse_order_acks(fixture("rest_cancel_batch.json"), storage);
    ASSERT_TRUE(batch.has_value());
    ASSERT_EQ(batch->size(), 2U);
    EXPECT_TRUE((*batch)[0].success);
    // mixed success is the normal batch shape
    EXPECT_FALSE((*batch)[1].success);
    EXPECT_TRUE(is_benign_cancel_error((*batch)[1].error_code));
    EXPECT_FALSE(is_benign_cancel_error((*batch)[0].error_code));
}

TEST(WeexWire, LeavesIsASubtractionBecauseTheStatusWordWillNotSayIt) {
    // weex has no PARTIALLY_FILLED; a half-filled order is NEW
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

    // 80 of 200 lots filled, 120 left, still NEW
    const auto working = map_open_order((*orders)[1], table);
    ASSERT_TRUE(working.has_value());
    EXPECT_EQ(working->status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(working->leaves.raw, 120);

    // edited: executedQty raised to origQty
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
    // edited: one field, since no capture of a weex order exists
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
    EXPECT_EQ(report->status, OrderStatus::Accepted);
}

TEST(WeexWire, AWordThisAdapterDoesNotKnowFallsBackToTheQuantities) {
    // an unknown status word is read by its quantities
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
    EXPECT_TRUE(report->fee.is_undef());
    EXPECT_EQ(report->qty.raw, 100);
}

TEST(WeexWire, PositionsAreSignedByTheirSideAndNettedPerInstrument) {
    // size is unsigned; separated mode sends a row per direction
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
            // 200 long minus 50 short
            EXPECT_EQ(report.net_qty.raw, 150);
            // the later row's timestamp
            EXPECT_EQ(report.ts_venue.ns, 1'764'588'999'999ULL * 1'000'000ULL);
        } else {
            EXPECT_EQ(report.instrument, doge);
            EXPECT_EQ(report.net_qty.raw, -15); // 1500 short against a lot of 100
        }
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

    // an omitted column is undef
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
    // income is a collateral change: received positive, paid negative
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

    EXPECT_FALSE(is_funding_bill((*bills)[2]));
    EXPECT_FALSE(map_funding_charge((*bills)[2], table, k_ingest).has_value());
}

TEST(WeexWire, AnUnreadableAmountIsNotAChargeOfZero) {
    const auto table = load_table();
    const auto edited = without(fixture("rest_income.json"), R"("income":"0.08266646",)");
    static thread_local BillBuffer storage;
    const auto bills = parse_bills(edited, storage);
    ASSERT_TRUE(bills.has_value());
    EXPECT_FALSE(map_funding_charge((*bills)[0], table, k_ingest).has_value());
}

TEST(WeexWire, APrivateBodyThatIsAnErrorIsNeverReadAsAnEmptyAccount) {
    // refusal shapes must fail the list readers
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
    const auto now = parse_server_time(R"({"serverTime":1789325256837})");
    ASSERT_TRUE(now.has_value());
    EXPECT_EQ(*now, 1'789'325'256'837ULL);
    // zero means no correction
    EXPECT_EQ(parse_server_time(R"({"noSuchField":1})").value_or(1), 0ULL);
}

TEST(WeexBook, ReadsFieldsInAnyOrder) {
    static thread_local LevelBuffer storage;
    const auto wire = parse_depth(
        R"({"a":[["2","1"]],"b":[["1","3"],["0.5","1"]],"u":12,"x":[1,[2]],"d":"CHANGED",)"
        R"("U":11,"s":"ETHUSDT","e":"depth","E":7})",
        storage);
    ASSERT_TRUE(wire.has_value());
    EXPECT_FALSE(wire->is_snapshot);
    EXPECT_EQ(wire->symbol, "ETHUSDT");
    EXPECT_EQ(wire->first_id, 11U);
    EXPECT_EQ(wire->last_id, 12U);
    EXPECT_EQ(wire->event_ms, 7U);
    EXPECT_EQ(wire->tier, 0U);
    ASSERT_EQ(wire->bids.size(), 2U);
    EXPECT_EQ(wire->bids[1].price, "0.5");
    ASSERT_EQ(wire->asks.size(), 1U);
    EXPECT_FALSE(parse_depth(R"({"a":[],"b":[],"u":2,"d":"CHANGED","U":1,"e":"depth"})", storage)
                     .has_value())
        << "no symbol";

    PrintBuffer prints;
    const auto trades =
        parse_trades(R"({"d":[{"m":true,"q":"0.5","x":{},"p":"2","T":3}],"e":"trade"})", prints);
    ASSERT_TRUE(trades.has_value());
    ASSERT_EQ(trades->size(), 1U);
    EXPECT_EQ((*trades)[0].price, "2");
    EXPECT_EQ((*trades)[0].qty, "0.5");
    EXPECT_EQ((*trades)[0].trade_id, "");
    EXPECT_EQ((*trades)[0].trade_ms, 3U);
    EXPECT_TRUE((*trades)[0].buyer_is_maker);
}

} // namespace
} // namespace raikiri::venues::weex
