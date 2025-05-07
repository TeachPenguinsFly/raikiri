#include <raikiri/venues/bybit/book_sync.hpp>
#include <raikiri/venues/bybit/errors.hpp>
#include <raikiri/venues/bybit/wire.hpp>
#include <raikiri/venues/bybit/ws.hpp>

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

namespace raikiri::venues::bybit {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::string{RAIKIRI_BYBIT_FIXTURES} + "/" + std::string{name};
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    auto text = buf.str();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

TEST(BybitTopic, DispatchesEveryFixture) {
    struct Case {
        const char* file;
        TopicKind kind;
        const char* symbol;
    };

    const Case cases[] = {
        {"orderbook_snapshot.json", TopicKind::Orderbook, "BTCUSDT"},
        {"orderbook_delta.json", TopicKind::Orderbook, "BTCUSDT"},
        {"public_trade.json", TopicKind::PublicTrade, "BTCUSDT"},
        {"tickers_snapshot.json", TopicKind::Tickers, "BTCUSDT"},
        {"tickers_delta.json", TopicKind::Tickers, "BTCUSDT"},
        {"order_update.json", TopicKind::Order, ""},
        {"execution.json", TopicKind::Execution, ""},
        {"position.json", TopicKind::Position, ""},
        {"op_response.json", TopicKind::OpResponse, ""},
        {"pong.json", TopicKind::Pong, ""},
    };
    for (const auto& c : cases) {
        const auto frame = fixture(c.file);
        const auto topic = parse_topic(frame);
        ASSERT_TRUE(topic.has_value()) << c.file << ": " << to_string(topic.error());
        EXPECT_EQ(topic->kind, c.kind) << c.file;
        EXPECT_EQ(topic->symbol, c.symbol) << c.file;
    }
}

TEST(BybitOrderbook, SnapshotCarriesLevelsAndOrderingIds) {
    const auto frame = fixture("orderbook_snapshot.json");
    OrderbookBuffer storage;
    const auto book = parse_orderbook(frame, storage);
    ASSERT_TRUE(book.has_value()) << to_string(book.error());

    EXPECT_EQ(book->symbol, "BTCUSDT");
    EXPECT_TRUE(book->is_snapshot);
    EXPECT_EQ(book->update_id, 18521288U);
    EXPECT_EQ(book->seq, 7961638724U);
    EXPECT_EQ(book->ts_ms, 1672304484978U);
    ASSERT_EQ(book->bids.size(), 3U);
    ASSERT_EQ(book->asks.size(), 2U);
    EXPECT_EQ(book->bids[0].price, "16493.50");
    EXPECT_EQ(book->bids[0].size, "0.006");
    EXPECT_EQ(book->asks[1].price, "16612.00");

    // Zero copy: every view must point inside the caller's own buffer, not
    // into scratch memory the next frame overwrites.
    const auto* begin = frame.data();
    const auto* end = begin + frame.size();
    EXPECT_GE(book->bids[0].price.data(), begin);
    EXPECT_LE(book->bids[0].price.data() + book->bids[0].price.size(), end);
    EXPECT_GE(book->symbol.data(), begin);
}

TEST(BybitOrderbook, DeltaKeepsZeroSizeDeleteVerbatim) {
    const auto frame = fixture("orderbook_delta.json");
    OrderbookBuffer storage;
    const auto book = parse_orderbook(frame, storage);
    ASSERT_TRUE(book.has_value());

    EXPECT_FALSE(book->is_snapshot);
    EXPECT_EQ(book->update_id, 18521289U);
    ASSERT_EQ(book->bids.size(), 2U);
    // "0" means delete; the parser passes it through and the book layer
    // decides — interpreting it here would put venue semantics in the codec.
    EXPECT_EQ(book->bids[0].size, "0");
    EXPECT_EQ(book->bids[1].size, "0.200");
}

// The two orderbook fixtures are consecutive frames off one socket, so the
// snapshot and the delta chained onto it are exercised the way the venue
// actually delivers them.
BookSync book_for_btcusdt() {
    return BookSync{InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{0}),
                    *parse_decimal_e9("0.1"),
                    *parse_decimal_e9("0.001")};
}

TEST(BybitBook, DepthSnapshotCrossesIntoTheDomain) {
    auto sync = book_for_btcusdt();
    OrderbookBuffer storage;
    const auto snapshot_frame = fixture("orderbook_snapshot.json");
    const auto wire = parse_orderbook(snapshot_frame, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(sync.apply(*wire), SyncResult::Resynced);

    const auto book = sync.snapshot(UnixNanos{1'700'000'000'000'000'000ULL}, UnixNanos{2});
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->bid_count, 3U);
    EXPECT_EQ(book->ask_count, 2U);
    EXPECT_EQ(book->venue_seq, 18521288U) << "update_id, so a wrong book is traceable";

    // Bids descend, asks ascend, and the two never cross — a book failing this
    // is not a book, and every strategy reading it would price off the fiction.
    EXPECT_LT(book->bids[0].price.raw, book->asks[0].price.raw);
    for (std::uint32_t i = 1; i < book->bid_count; ++i) {
        EXPECT_LT(book->bids[i].price.raw, book->bids[i - 1].price.raw) << i;
    }
    for (std::uint32_t i = 1; i < book->ask_count; ++i) {
        EXPECT_GT(book->asks[i].price.raw, book->asks[i - 1].price.raw) << i;
    }
    for (std::uint32_t i = 0; i < book->bid_count; ++i) {
        EXPECT_GT(book->bids[i].qty.raw, 0) << i;
    }
    for (std::uint32_t i = 0; i < book->ask_count; ++i) {
        EXPECT_GT(book->asks[i].qty.raw, 0) << i;
    }
    EXPECT_EQ(book->bids[0].price.raw, 164935); // 16493.50 / 0.1
    EXPECT_EQ(book->bids[0].qty.raw, 6);        // 0.006 / 0.001

    OrderbookBuffer delta_storage;
    const auto delta_frame = fixture("orderbook_delta.json");
    const auto delta = parse_orderbook(delta_frame, delta_storage);
    ASSERT_TRUE(delta.has_value());
    ASSERT_EQ(sync.apply(*delta), SyncResult::Applied);

    const auto after = sync.snapshot(UnixNanos{3}, UnixNanos{4});
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->bid_count, 2U) << "the top bid was deleted by size 0";
    EXPECT_EQ(after->bids[0].price.raw, 164930);
    EXPECT_EQ(after->bids[0].qty.raw, 200);
    EXPECT_EQ(after->venue_seq, 18521289U);
}

TEST(BybitBook, EverySubscribedLevelReachesTheDomain) {
    // A live orderbook.50 snapshot, so this fails if the venue quietly serves
    // a shallower book than the topic asked for or the book layer drops the
    // tail on its way into shm.
    auto sync = book_for_btcusdt();
    OrderbookBuffer storage;
    const auto frame = fixture("orderbook_snapshot_50.json");
    const auto wire = parse_orderbook(frame, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->bids.size(), k_book_depth);
    ASSERT_EQ(sync.apply(*wire), SyncResult::Resynced);

    const auto book = sync.snapshot(UnixNanos{1}, UnixNanos{2});
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->bid_count, k_book_depth);
    EXPECT_EQ(book->ask_count, k_book_depth);
    static_assert(k_book_depth <= k_book_levels);
    EXPECT_EQ(book->bids[0].price.raw, 640'607); // 64060.70 / 0.1
    EXPECT_LT(book->bids[k_book_depth - 1].price.raw, book->bids[0].price.raw);
}

TEST(BybitBook, AGapInTheUpdateChainPublishesNothing) {
    auto sync = book_for_btcusdt();
    OrderbookBuffer storage;
    const auto snapshot_frame = fixture("orderbook_snapshot.json");
    const auto wire = parse_orderbook(snapshot_frame, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(sync.apply(*wire), SyncResult::Resynced);
    ASSERT_TRUE(sync.snapshot(UnixNanos{1}, UnixNanos{2}).has_value());

    OrderbookBuffer delta_storage;
    const auto delta_frame = fixture("orderbook_delta.json");
    auto delta = parse_orderbook(delta_frame, delta_storage);
    ASSERT_TRUE(delta.has_value());
    delta->update_id += 1; // the frame that would have carried 18521289 never arrived

    EXPECT_EQ(sync.apply(*delta), SyncResult::GapDetected);
    EXPECT_FALSE(sync.snapshot(UnixNanos{3}, UnixNanos{4}).has_value())
        << "a book with an unseen hole in it must publish nothing";
}

TEST(BybitTrades, BothAggressorSides) {
    const auto frame = fixture("public_trade.json");
    TradeBuffer storage;
    const auto trades = parse_trades(frame, storage);
    ASSERT_TRUE(trades.has_value()) << to_string(trades.error());
    ASSERT_EQ(trades->size(), 2U);

    EXPECT_EQ((*trades)[0].symbol, "BTCUSDT");
    EXPECT_EQ((*trades)[0].price, "16578.50");
    EXPECT_EQ((*trades)[0].size, "0.001");
    EXPECT_TRUE((*trades)[0].buyer_aggressor) << "\"S\":\"Buy\" is the taker lifting the offer";
    EXPECT_EQ((*trades)[0].ts_ms, 1672304486865U);
    EXPECT_EQ((*trades)[0].trade_id, "20f43950-d8dd-5b31-9112-a178eb6023af");
    EXPECT_FALSE((*trades)[1].buyer_aggressor);
}

TEST(BybitTicker, SnapshotPopulatesEverything) {
    const auto frame = fixture("tickers_snapshot.json");
    const auto ticker = parse_ticker(frame);
    ASSERT_TRUE(ticker.has_value()) << to_string(ticker.error());

    EXPECT_TRUE(ticker->is_snapshot);
    EXPECT_EQ(ticker->symbol, "BTCUSDT");
    EXPECT_EQ(ticker->mark_price, "17217.33");
    EXPECT_EQ(ticker->index_price, "17227.36");
    EXPECT_EQ(ticker->funding_rate, "-0.000212");
    EXPECT_EQ(ticker->next_funding_ms, "1673280000000");
    EXPECT_EQ(ticker->bid1_price, "17215.50");
    EXPECT_EQ(ticker->ask1_size, "83.020");
}

TEST(BybitTicker, DeltaLeavesAbsentFieldsEmptyRatherThanZero) {
    // The bug class this guards: a linear-perp ticker delta carries only
    // what changed. Reading an absent fundingRate as zero would wipe the
    // rate a funding strategy is pricing against.
    const auto frame = fixture("tickers_delta.json");
    const auto ticker = parse_ticker(frame);
    ASSERT_TRUE(ticker.has_value()) << to_string(ticker.error());

    EXPECT_FALSE(ticker->is_snapshot);
    EXPECT_EQ(ticker->symbol, "BTCUSDT");
    EXPECT_EQ(ticker->bid1_price, "17215.00") << "changed field present";
    EXPECT_TRUE(ticker->funding_rate.empty()) << "absent means unchanged, not zero";
    EXPECT_TRUE(ticker->mark_price.empty());
    EXPECT_TRUE(ticker->next_funding_ms.empty());
}

TEST(BybitOrders, FilledUpdate) {
    const auto frame = fixture("order_update.json");
    OrderUpdateBuffer storage;
    const auto updates = parse_order_updates(frame, storage);
    ASSERT_TRUE(updates.has_value()) << to_string(updates.error());
    ASSERT_EQ(updates->size(), 1U);

    const auto& u = (*updates)[0];
    EXPECT_EQ(u.symbol, "BTCUSDT");
    EXPECT_EQ(u.order_id, "5cf98598-4a94-4f4c-b1c5-4a35a4e3f9a3");
    EXPECT_EQ(u.order_link_id, "01ARZ3NDEKTSV4RRFFQ69G5FAV") << "our ULID returns intact";
    EXPECT_EQ(u.status, WireOrderStatus::Filled);
    EXPECT_EQ(u.leaves_qty, "0");
    EXPECT_EQ(u.cum_exec_qty, "1");
    EXPECT_EQ(u.updated_ms, 1672364262457U) << "ms inside a JSON string";
}

TEST(BybitOrders, RejectedCarriesItsReason) {
    const auto frame = fixture("order_rejected.json");
    OrderUpdateBuffer storage;
    const auto updates = parse_order_updates(frame, storage);
    ASSERT_TRUE(updates.has_value());
    ASSERT_EQ(updates->size(), 1U);

    EXPECT_EQ((*updates)[0].status, WireOrderStatus::Rejected);
    EXPECT_EQ((*updates)[0].reject_reason, "EC_PostOnlyWillTakeLiquidity");
    EXPECT_TRUE((*updates)[0].order_id.empty()) << "a rejected order got no venue id";
}

TEST(BybitOrders, StatusMappingIncludingTheTerminalTrap) {
    EXPECT_EQ(order_status_from("New"), WireOrderStatus::New);
    EXPECT_EQ(order_status_from("PartiallyFilled"), WireOrderStatus::PartiallyFilled);
    EXPECT_EQ(order_status_from("Filled"), WireOrderStatus::Filled);
    EXPECT_EQ(order_status_from("Cancelled"), WireOrderStatus::Cancelled);
    // Reads like a partial fill, is terminal — mapping it to PartiallyFilled
    // would leave the order open in our store forever.
    EXPECT_EQ(order_status_from("PartiallyFilledCanceled"),
              WireOrderStatus::PartiallyFilledCanceled);
    EXPECT_EQ(order_status_from("SomethingNew"), WireOrderStatus::Unknown);
    EXPECT_EQ(order_status_from(""), WireOrderStatus::Unknown);
}

TEST(BybitExecutions, FillCarriesFeeAndLiquidity) {
    const auto frame = fixture("execution.json");
    ExecutionBuffer storage;
    const auto execs = parse_executions(frame, storage);
    ASSERT_TRUE(execs.has_value()) << to_string(execs.error());
    ASSERT_EQ(execs->size(), 1U);

    const auto& e = (*execs)[0];
    EXPECT_EQ(e.exec_id, "7e2ae69c-4edf-5800-a352-893d52b446aa");
    EXPECT_EQ(e.order_link_id, "01ARZ3NDEKTSV4RRFFQ69G5FAV");
    EXPECT_EQ(e.exec_price, "16618.20");
    EXPECT_EQ(e.exec_qty, "0.00012");
    EXPECT_EQ(e.exec_fee, "0.005061");
    EXPECT_FALSE(e.is_maker) << "taker fill";
    EXPECT_EQ(e.leaves_qty, "0");
    EXPECT_EQ(e.exec_ms, 1672364174443U);
}

TEST(BybitPositions, ParsesSideAndEntry) {
    const auto frame = fixture("position.json");
    PositionBuffer storage;
    const auto positions = parse_positions(frame, storage);
    ASSERT_TRUE(positions.has_value()) << to_string(positions.error());
    ASSERT_EQ(positions->size(), 1U);

    EXPECT_EQ((*positions)[0].symbol, "BTCUSDT");
    EXPECT_EQ((*positions)[0].side, "Buy");
    EXPECT_EQ((*positions)[0].size, "0.010");
    EXPECT_EQ((*positions)[0].avg_price, "16618.20");
}

TEST(BybitRest, EnvelopeSuccessAndOrderAck) {
    const auto body = fixture("rest_order_ack.json");
    const auto env = parse_envelope(body);
    ASSERT_TRUE(env.has_value()) << to_string(env.error());
    EXPECT_EQ(env->ret_code, 0);
    EXPECT_EQ(env->ret_msg, "OK");
    EXPECT_EQ(env->time_ms, 1672211918471U);
    ASSERT_FALSE(env->result.empty());

    const auto ack = parse_order_ack(env->result);
    ASSERT_TRUE(ack.has_value()) << to_string(ack.error());
    EXPECT_EQ(ack->order_id, "1321003749386327552");
    EXPECT_EQ(ack->order_link_id, "01ARZ3NDEKTSV4RRFFQ69G5FAV");
}

TEST(BybitRest, ErrorEnvelopeSurfacesCodeAndClassifies) {
    const auto body = fixture("rest_error.json");
    const auto env = parse_envelope(body);
    ASSERT_TRUE(env.has_value());
    EXPECT_EQ(env->ret_code, 110007);
    EXPECT_EQ(env->ret_msg, "ab not enough for new order");
    // Insufficient balance is terminal: retrying cannot conjure funds.
    EXPECT_EQ(classify(env->ret_code), exec::ErrorClass::Terminal);
}

TEST(BybitRest, OpenOrdersList) {
    const auto body = fixture("rest_open_orders.json");
    OrderUpdateBuffer storage;
    const auto orders = parse_open_orders(body, storage);
    ASSERT_TRUE(orders.has_value()) << to_string(orders.error());
    ASSERT_EQ(orders->size(), 1U);
    EXPECT_EQ((*orders)[0].order_link_id, "01ARZ3NDEKTSV4RRFFQ69G5FAV");
    EXPECT_EQ((*orders)[0].status, WireOrderStatus::New);
    EXPECT_EQ((*orders)[0].leaves_qty, "0.001");
}

TEST(BybitRest, InstrumentsReachIntoNestedFilters) {
    // tickSize and qtyStep live in NESTED objects — the shape a flat
    // field-by-name loader silently misses, and an instrument with a zero
    // tick size divides by zero at order time.
    const auto body = fixture("rest_instruments.json");
    InstrumentBuffer storage;
    const auto instruments = parse_instruments(body, storage);
    ASSERT_TRUE(instruments.has_value()) << to_string(instruments.error());
    ASSERT_EQ(instruments->size(), 2U);

    EXPECT_EQ((*instruments)[0].symbol, "BTCUSDT");
    EXPECT_EQ((*instruments)[0].status, "Trading");
    EXPECT_EQ((*instruments)[0].base_coin, "BTC");
    EXPECT_EQ((*instruments)[0].tick_size, "0.10");
    EXPECT_EQ((*instruments)[0].qty_step, "0.001");
    EXPECT_EQ((*instruments)[0].min_order_qty, "0.001");
    EXPECT_EQ((*instruments)[0].min_notional, "5");
    EXPECT_EQ((*instruments)[1].symbol, "ETHUSDT");
    EXPECT_EQ((*instruments)[1].tick_size, "0.01");
}

TEST(BybitErrors, ClassificationBuckets) {
    EXPECT_EQ(classify(k_ret_rate_limit), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(k_ret_ip_banned), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(k_ret_sign_error), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify(k_ret_invalid_api_key), exec::ErrorClass::AuthFailure);
    // Clock skew looks like a signing failure but is fixed by resyncing
    // time, so the clock poller gets a chance before we give up.
    EXPECT_EQ(classify(k_ret_timestamp_error), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_ret_insufficient_balance), exec::ErrorClass::Terminal);
    // An unrecognised refusal repeated at full speed earns a ban; terminal
    // is the safe default because reconciliation can undo it.
    EXPECT_EQ(classify(999999), exec::ErrorClass::Terminal);

    EXPECT_TRUE(is_benign_cancel_failure(k_ret_order_not_exists));
    EXPECT_FALSE(is_benign_cancel_failure(k_ret_insufficient_balance));
}

TEST(BybitWire, MalformedInputIsRefusedNotCrashed) {
    const std::string_view bad[] = {
        "",
        "{",
        "not json at all",
        "[]",
        "{}",
        R"({"topic":123})",
        R"({"topic":"orderbook.1.BTCUSDT","data":"not-an-object"})",
        R"({"topic":"publicTrade.BTCUSDT","data":{}})",
        R"({"topic":"orderbook.1.BTCUSDT","data":{"s":"BTCUSDT","b":[["only-one-element"]]}})",
        R"({"retCode":"not-a-number"})",
    };
    OrderbookBuffer ob;
    TradeBuffer tb;
    OrderUpdateBuffer ou;
    ExecutionBuffer eb;
    for (const auto frame : bad) {
        // The contract is only that every one of these returns rather than
        // crashing or reading out of bounds; ASan is the real assertion.
        (void)parse_topic(frame);
        (void)parse_orderbook(frame, ob);
        (void)parse_trades(frame, tb);
        (void)parse_ticker(frame);
        (void)parse_order_updates(frame, ou);
        (void)parse_executions(frame, eb);
        (void)parse_envelope(frame);
        (void)parse_order_ack(frame);
        (void)parse_open_orders(frame, ou);
    }
    SUCCEED();
}

TEST(BybitWire, TruncationsOfEveryFixtureAreRefused) {
    // Byte-wise truncation is the cheapest fuzz there is and it catches the
    // "parser reads past the end of a half-frame" class outright.
    for (const auto* name : {"orderbook_snapshot.json",
                             "public_trade.json",
                             "tickers_snapshot.json",
                             "order_update.json",
                             "execution.json"}) {
        const auto full = fixture(name);
        OrderbookBuffer ob;
        TradeBuffer tb;
        OrderUpdateBuffer ou;
        ExecutionBuffer eb;
        for (std::size_t len = 1; len < full.size(); ++len) {
            const std::string_view frame{full.data(), len};
            (void)parse_topic(frame);
            (void)parse_orderbook(frame, ob);
            (void)parse_trades(frame, tb);
            (void)parse_ticker(frame);
            (void)parse_order_updates(frame, ou);
            (void)parse_executions(frame, eb);
        }
    }
    SUCCEED();
}

} // namespace
} // namespace raikiri::venues::bybit
