#include <raikiri/venues/okx/book.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/venues/okx/map.hpp>
#include <raikiri/venues/okx/rest.hpp>
#include <raikiri/venues/okx/wire.hpp>
#include <raikiri/venues/okx/ws.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::okx {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{RAIKIRI_OKX_FIXTURES} / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

TEST(OkxWire, EventDispatchResolvesKindAndInstrument) {
    for (const auto& [file, kind] : {std::pair{"ws_bbo_tbt.json", EventKind::Bbo},
                                     std::pair{"ws_trades.json", EventKind::Trade},
                                     std::pair{"ws_funding_rate.json", EventKind::FundingRate},
                                     std::pair{"ws_books_snapshot.json", EventKind::Books}}) {
        const auto frame = fixture(file);
        const auto event = parse_event(frame);
        ASSERT_TRUE(event.has_value()) << file;
        EXPECT_EQ(event->kind, kind) << file;
        EXPECT_EQ(event->inst_id, "BTC-USDT-SWAP") << file;
    }
}

TEST(OkxWire, ControlFramesAreNotMistakenForData) {
    // The two answers differ by one field value. Reading a refusal as an ack
    // turns "the venue rejected this channel" into "this channel is quiet",
    // which is indistinguishable from a market with no activity.
    const auto ack = fixture("ws_subscribe_ack.json");
    auto event = parse_event(ack);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::SubscribeAck);

    const auto bad = fixture("ws_error.json");
    event = parse_event(bad);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::Error);

    const auto err = parse_stream_error(bad);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, 60018);
    EXPECT_FALSE(err->msg.empty());
}

TEST(OkxWire, KeepaliveAnswerIsNotJson) {
    // OKX answers the "ping" text with a bare "pong" — not JSON, so it lands
    // on the same ignore path as any frame the venue adds later.
    EXPECT_FALSE(parse_event("pong").has_value());
}

TEST(OkxWire, BboParsesBothSides) {
    const auto frame = fixture("ws_bbo_tbt.json");
    const auto bbo = parse_bbo(frame);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->inst_id, "BTC-USDT-SWAP");
    EXPECT_FALSE(bbo->bid_price.empty());
    EXPECT_FALSE(bbo->ask_price.empty());
    EXPECT_FALSE(bbo->bid_qty.empty());
    EXPECT_FALSE(bbo->ask_qty.empty());
    EXPECT_GT(bbo->seq_id, 0);
    // Milliseconds out of a digit string, not a JSON number: running this
    // through the decimal parser would scale it by 1e9 and overflow.
    EXPECT_GT(bbo->ts_ms, 1'700'000'000'000U);

    // Views point into the caller's buffer, not into parser scratch.
    EXPECT_GE(bbo->bid_price.data(), frame.data());
    EXPECT_LE(bbo->bid_price.data() + bbo->bid_price.size(), frame.data() + frame.size());
}

TEST(OkxWire, TradesCarryTakerSideDirectly) {
    auto storage = std::make_unique<TradeBuffer>();
    const auto frame = fixture("ws_trades.json");
    const auto trades = parse_trades(frame, *storage);
    ASSERT_TRUE(trades.has_value());
    ASSERT_EQ(trades->size(), 1U);
    const auto& trade = trades->front();
    EXPECT_EQ(trade.inst_id, "BTC-USDT-SWAP");
    EXPECT_FALSE(trade.trade_id.empty());
    EXPECT_FALSE(trade.price.empty());
    EXPECT_FALSE(trade.qty.empty());
    EXPECT_GT(trade.ts_ms, 1'700'000'000'000U);
    // "sell" in the captured frame, and it already means the AGGRESSOR sold —
    // no inversion, unlike binance's buyer-was-maker flag.
    EXPECT_FALSE(trade.aggressor_bought);
}

TEST(OkxWire, FundingRateSeparatesCurrentSettlementFromTheOneAfter) {
    const auto frame = fixture("ws_funding_rate.json");
    const auto funding = parse_funding_rate(frame);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->inst_id, "BTC-USDT-SWAP");
    EXPECT_FALSE(funding->funding_rate.empty());
    // fundingTime is the settlement the current rate applies to; the field
    // named "next" is one interval further out. Swapping them puts every
    // countdown an interval into the future.
    EXPECT_GT(funding->funding_ms, 1'700'000'000'000U);
    EXPECT_GT(funding->next_funding_ms, funding->funding_ms);
    // Observed live: OKX ships the forecast field empty rather than omitting
    // it, so the mapper must not read "" as a rate of zero.
    EXPECT_TRUE(funding->next_funding_rate.empty());
}

TEST(OkxWire, BooksSnapshotAndDeltasChainOnSeqId) {
    auto storage = std::make_unique<LevelBuffer>();
    const auto snapshot_frame = fixture("ws_books_snapshot.json");
    const auto snapshot = parse_books(snapshot_frame, *storage);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_TRUE(snapshot->snapshot);
    EXPECT_EQ(snapshot->bids.size(), 400U);
    EXPECT_EQ(snapshot->asks.size(), 400U);
    // -1, which is why both id fields are signed.
    EXPECT_LT(snapshot->prev_seq_id, 0);
    const auto snapshot_seq = snapshot->seq_id;

    auto next_storage = std::make_unique<LevelBuffer>();
    const auto first_frame = fixture("ws_books_update.json");
    const auto first = parse_books(first_frame, *next_storage);
    ASSERT_TRUE(first.has_value());
    EXPECT_FALSE(first->snapshot);
    // The chaining rule: prevSeqId equals the previous message's seqId. These
    // three fixtures are consecutive frames off one live socket, and the
    // checksum they still carry is pinned to 0 since 2026-06-23 — it is not
    // surfaced at all, so nothing can be tempted to validate against it.
    EXPECT_EQ(first->prev_seq_id, snapshot_seq);

    auto third_storage = std::make_unique<LevelBuffer>();
    const auto second_frame = fixture("ws_books_update_next.json");
    const auto second = parse_books(second_frame, *third_storage);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->prev_seq_id, first->seq_id);
}

// BTC-USDT-SWAP as OKX lists it: 0.1 tick, 0.01 lot. The three books fixtures
// are consecutive frames off one socket, so the book is driven the way the
// venue actually delivers it rather than from invented deltas.
Book btc_book() {
    return Book{InstrumentId::make(VenueId::Okx, MarketType::Perp, SymbolId{0}),
                *parse_decimal_e9("0.1"),
                *parse_decimal_e9("0.01")};
}

TEST(OkxBook, DepthSnapshotCrossesIntoTheDomain) {
    auto book = btc_book();
    auto storage = std::make_unique<LevelBuffer>();
    const auto snapshot_frame = fixture("ws_books_snapshot.json");
    const auto wire = parse_books(snapshot_frame, *storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(book.apply(*wire), BookResult::Resynced);

    const auto depth = book.snapshot(UnixNanos{1'700'000'000'000'000'000ULL}, UnixNanos{2});
    ASSERT_TRUE(depth.has_value());
    EXPECT_EQ(depth->bid_count, k_book_levels) << "400 levels a side, cut to what shm carries";
    EXPECT_EQ(depth->ask_count, k_book_levels);
    EXPECT_EQ(depth->venue_seq, 333'626'642'610ULL) << "seqId, so a wrong book is traceable";

    // Bids descend, asks ascend, and the two never cross — a book failing this
    // is not a book, and every strategy reading it would price off the fiction.
    EXPECT_LT(depth->bids[0].price.raw, depth->asks[0].price.raw);
    for (std::uint32_t i = 1; i < depth->bid_count; ++i) {
        EXPECT_LT(depth->bids[i].price.raw, depth->bids[i - 1].price.raw) << i;
    }
    for (std::uint32_t i = 1; i < depth->ask_count; ++i) {
        EXPECT_GT(depth->asks[i].price.raw, depth->asks[i - 1].price.raw) << i;
    }
    for (std::uint32_t i = 0; i < depth->bid_count; ++i) {
        EXPECT_GT(depth->bids[i].qty.raw, 0) << i;
        EXPECT_GT(depth->asks[i].qty.raw, 0) << i;
    }
    EXPECT_EQ(depth->bids[0].price.raw, 640'435); // 64043.5 / 0.1
    EXPECT_EQ(depth->bids[0].qty.raw, 29'867);    // 298.67 / 0.01
    EXPECT_EQ(depth->asks[0].price.raw, 640'436);

    auto delta_storage = std::make_unique<LevelBuffer>();
    const auto delta_frame = fixture("ws_books_update.json");
    const auto delta = parse_books(delta_frame, *delta_storage);
    ASSERT_TRUE(delta.has_value());
    ASSERT_EQ(book.apply(*delta), BookResult::Applied);

    const auto after = book.snapshot(UnixNanos{3}, UnixNanos{4});
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->venue_seq, 333'626'642'622ULL);
    EXPECT_EQ(after->bids[0].price.raw, 640'435) << "the delta moved levels away from the touch";
    EXPECT_LT(after->bids[0].price.raw, after->asks[0].price.raw);
}

TEST(OkxBook, ASeqIdGapPublishesNothing) {
    // The middle frame never arrives: the third's prevSeqId names a message
    // this book has not seen, so everything after it would be guesswork.
    auto book = btc_book();
    auto storage = std::make_unique<LevelBuffer>();
    const auto snapshot_frame = fixture("ws_books_snapshot.json");
    const auto wire = parse_books(snapshot_frame, *storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(book.apply(*wire), BookResult::Resynced);
    ASSERT_TRUE(book.snapshot(UnixNanos{1}, UnixNanos{2}).has_value());

    auto skipped_storage = std::make_unique<LevelBuffer>();
    const auto skipped_frame = fixture("ws_books_update_next.json");
    const auto skipped = parse_books(skipped_frame, *skipped_storage);
    ASSERT_TRUE(skipped.has_value());
    EXPECT_EQ(book.apply(*skipped), BookResult::GapDetected);
    EXPECT_EQ(book.gaps(), 1U);
    EXPECT_FALSE(book.synced());
    EXPECT_FALSE(book.snapshot(UnixNanos{3}, UnixNanos{4}).has_value())
        << "a book with an unseen hole in it must publish nothing";

    // Recovery is the venue's next snapshot, which resubscribing asks for.
    auto again = std::make_unique<LevelBuffer>();
    const auto fresh = parse_books(snapshot_frame, *again);
    ASSERT_TRUE(fresh.has_value());
    EXPECT_EQ(book.apply(*fresh), BookResult::Resynced);
    EXPECT_TRUE(book.snapshot(UnixNanos{5}, UnixNanos{6}).has_value());
}

TEST(OkxBook, ARedeliveredDeltaIsNotMistakenForAGap) {
    auto book = btc_book();
    auto storage = std::make_unique<LevelBuffer>();
    const auto snapshot_frame = fixture("ws_books_snapshot.json");
    const auto wire = parse_books(snapshot_frame, *storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(book.apply(*wire), BookResult::Resynced);

    auto delta_storage = std::make_unique<LevelBuffer>();
    const auto delta_frame = fixture("ws_books_update.json");
    const auto delta = parse_books(delta_frame, *delta_storage);
    ASSERT_TRUE(delta.has_value());
    ASSERT_EQ(book.apply(*delta), BookResult::Applied);

    EXPECT_EQ(book.apply(*delta), BookResult::Ignored) << "already applied, not a hole";
    EXPECT_EQ(book.gaps(), 0U);
    EXPECT_TRUE(book.synced());
}

TEST(OkxBook, LevelsOffTheStepGridPublishNothing) {
    // A book whose prices no longer divide by the instrument's tick means OKX
    // moved a precision. Rounding into the grid would invent quotes the venue
    // never published, so the book is dropped and awaits a fresh snapshot.
    Book book{InstrumentId::make(VenueId::Okx, MarketType::Perp, SymbolId{0}),
              *parse_decimal_e9("1"),
              *parse_decimal_e9("0.01")};
    auto storage = std::make_unique<LevelBuffer>();
    const auto snapshot_frame = fixture("ws_books_snapshot.json");
    const auto wire = parse_books(snapshot_frame, *storage);
    ASSERT_TRUE(wire.has_value());

    EXPECT_EQ(book.apply(*wire), BookResult::BadLevel) << "64043.5 is half a tick";
    EXPECT_FALSE(book.synced());
    EXPECT_FALSE(book.snapshot(UnixNanos{1}, UnixNanos{2}).has_value());
}

TEST(OkxBook, ADeltaBeforeAnySnapshotIsIgnored) {
    auto book = btc_book();
    auto storage = std::make_unique<LevelBuffer>();
    const auto frame = fixture("ws_books_update.json");
    const auto delta = parse_books(frame, *storage);
    ASSERT_TRUE(delta.has_value());
    EXPECT_EQ(book.apply(*delta), BookResult::Ignored);
    EXPECT_FALSE(book.snapshot(UnixNanos{1}, UnixNanos{2}).has_value());
}

TEST(OkxWire, DeletedLevelsKeepTheirZeroQuantity) {
    auto storage = std::make_unique<LevelBuffer>();
    const auto frame = fixture("ws_books_update_next.json");
    const auto books = parse_books(frame, *storage);
    ASSERT_TRUE(books.has_value());
    for (const auto& level : books->asks) {
        EXPECT_FALSE(level.price.empty());
        EXPECT_FALSE(level.qty.empty()); // "0" is a delete, never an absence
    }
}

TEST(OkxWire, RestEnvelopeSeparatesSuccessFromRefusal) {
    for (const char* file : {"rest_instruments.json",
                             "rest_mark_price.json",
                             "rest_funding_rate.json",
                             "rest_server_time.json"}) {
        const auto body = fixture(file);
        const auto envelope = parse_rest_envelope(body);
        ASSERT_TRUE(envelope.has_value()) << file;
        EXPECT_EQ(envelope->code, k_ok) << file;
    }

    const auto body = fixture("rest_error_bad_instrument.json");
    const auto envelope = parse_rest_envelope(body);
    ASSERT_TRUE(envelope.has_value());
    EXPECT_EQ(envelope->code, 51001);
    EXPECT_FALSE(envelope->msg.empty());
}

TEST(OkxWire, UnreadableEnvelopeIsNotReadAsSuccess) {
    // Zero means success, so a code that is not a run of digits must fail
    // rather than decay into one.
    EXPECT_FALSE(parse_rest_envelope(R"({"code":"oops","msg":"","data":[]})").has_value());
    EXPECT_FALSE(parse_rest_envelope(R"({"msg":"","data":[]})").has_value());
}

TEST(OkxWire, InstrumentsReportWhatTheVenueSaid) {
    auto storage = std::make_unique<InstrumentBuffer>();
    const auto body = fixture("rest_instruments.json");
    const auto rows = parse_instruments(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);

    const auto find = [&](std::string_view inst_id) -> const WireInstrument* {
        for (const auto& row : *rows) {
            if (row.inst_id == inst_id) {
                return &row;
            }
        }
        return nullptr;
    };

    const auto* btc = find("BTC-USDT-SWAP");
    ASSERT_NE(btc, nullptr);
    EXPECT_EQ(btc->inst_type, "SWAP");
    EXPECT_EQ(btc->state, "live");
    EXPECT_EQ(btc->tick_size, "0.1");
    EXPECT_EQ(btc->lot_size, "0.01");
    EXPECT_EQ(btc->ct_val, "0.01"); // one contract is 0.01 BTC, not 1
    EXPECT_EQ(btc->ct_mult, "1");

    // The wire layer reports; deciding a preopen row or a dated future is
    // unusable belongs to the mapper. A preopen row ships every numeric field
    // as an empty string, which is why they are optional here.
    const auto* preopen = find("JP225-USDT-SWAP");
    ASSERT_NE(preopen, nullptr);
    EXPECT_EQ(preopen->state, "preopen");
    EXPECT_TRUE(preopen->tick_size.empty());
    const auto* dated = find("BTC-USD-260814");
    ASSERT_NE(dated, nullptr);
    EXPECT_EQ(dated->inst_type, "FUTURES");
}

TEST(OkxWire, MarkPriceAndServerTimeParse) {
    // The body is held, not passed as a temporary: every view a parser returns
    // points into the caller's buffer, so outliving it is the contract.
    const auto mark_body = fixture("rest_mark_price.json");
    const auto mark = parse_mark_price(mark_body);
    ASSERT_TRUE(mark.has_value());
    EXPECT_EQ(mark->inst_id, "BTC-USDT-SWAP");
    EXPECT_FALSE(mark->mark_price.empty());

    const auto time = parse_server_time(fixture("rest_server_time.json"));
    ASSERT_TRUE(time.has_value());
    EXPECT_GT(*time, 1'700'000'000'000U);
}

TEST(OkxWire, EmptyDataArrayIsNotADatum) {
    // A refusal carries data:[]; inventing a zero-valued reading from it is
    // how a stale mark price gets published as a real one.
    EXPECT_FALSE(parse_mark_price(R"({"code":"51001","msg":"nope","data":[]})").has_value());
    EXPECT_FALSE(parse_server_time(R"({"code":"0","msg":"","data":[]})").has_value());
}

TEST(OkxWire, GarbageIsRefusedRatherThanPartiallyRead) {
    auto levels = std::make_unique<LevelBuffer>();
    auto trades = std::make_unique<TradeBuffer>();
    for (const char* junk :
         {"",
          "{",
          "[]",
          "null",
          R"({"arg":)",
          "\xff\xfe",
          R"({"arg":{"channel":"bbo-tbt"}})",
          R"({"arg":{"channel":"bbo-tbt","instId":"BTC-USDT-SWAP"},"data":[{}]})"}) {
        const std::string_view frame{junk};
        (void)parse_event(frame);
        EXPECT_FALSE(parse_bbo(frame).has_value()) << junk;
        EXPECT_FALSE(parse_trades(frame, *trades).has_value()) << junk;
        EXPECT_FALSE(parse_funding_rate(frame).has_value()) << junk;
        EXPECT_FALSE(parse_books(frame, *levels).has_value()) << junk;
        EXPECT_FALSE(parse_server_time(frame).has_value()) << junk;
    }
}

// --------------------------------------------------------------- private ----

TEST(OkxWire, OrderChannelIsItsOwnEventKind) {
    const auto frame = fixture("ws_orders_fill.json");
    const auto event = parse_event(frame);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::Orders);
    EXPECT_EQ(event->inst_id, "BTC-USDT");
}

TEST(OkxWire, LoginAnswerIsNotASubscribeAck) {
    // Both arrive as {"event":...}. Reading a refused login as an ack leaves a
    // socket that answers every private subscribe with 60011 and no clue why.
    const auto accepted = fixture("ws_login_ok.json");
    auto event = parse_event(accepted);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::LoginAck);
    const auto ok = parse_stream_error(accepted);
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->code, k_ok);

    const auto refused = fixture("ws_login_failed.json");
    event = parse_event(refused);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::Error);
    const auto failed = parse_stream_error(refused);
    ASSERT_TRUE(failed.has_value());
    EXPECT_EQ(failed->code, k_ws_login_failed);
}

TEST(OkxWire, OrderPushCarriesLifecycleAndFillTogether) {
    const auto frame = fixture("ws_orders_fill.json");
    OrderBuffer storage{};
    const auto orders = parse_orders(frame, storage);
    ASSERT_TRUE(orders.has_value());
    ASSERT_EQ(orders->size(), 1U);

    const auto& order = orders->front();
    EXPECT_EQ(order.inst_id, "BTC-USDT");
    EXPECT_EQ(order.ord_id, "452197707845865472");
    EXPECT_EQ(order.state, WireOrderState::Filled);
    EXPECT_EQ(order.side, "sell");
    EXPECT_EQ(order.px, "31527.1");
    EXPECT_EQ(order.sz, "0.001");
    EXPECT_EQ(order.acc_fill_sz, "0.001");
    EXPECT_EQ(order.fill_px, "31527.1");
    EXPECT_EQ(order.fill_sz, "0.001");
    EXPECT_EQ(order.trade_id, "242589207");
    EXPECT_EQ(order.exec_type, "M");
    // Negative on the wire, and the mapper is what flips it.
    EXPECT_EQ(order.fill_fee, "-0.02522168");
    EXPECT_EQ(order.code, k_ok);
    EXPECT_EQ(order.fill_ms, 1'654'084'353'263ULL);
    EXPECT_EQ(order.u_ms, 1'654'084'353'264ULL);

    // Views point into the caller's buffer, not into parser scratch.
    EXPECT_GE(order.fill_px.data(), frame.data());
    EXPECT_LE(order.fill_px.data() + order.fill_px.size(), frame.data() + frame.size());
}

TEST(OkxWire, OneMessageIsBothTheLastFillAndTheCancel) {
    const auto frame = fixture("ws_orders_partial_expiry.json");
    OrderBuffer storage{};
    const auto orders = parse_orders(frame, storage);
    ASSERT_TRUE(orders.has_value());
    ASSERT_EQ(orders->size(), 1U);

    const auto& order = orders->front();
    EXPECT_EQ(order.state, WireOrderState::Canceled);
    EXPECT_FALSE(order.trade_id.empty()); // and it still carries a trade
    EXPECT_EQ(order.fill_sz, "2");
    EXPECT_EQ(order.sz, "5");
    EXPECT_EQ(order.acc_fill_sz, "2");
    // 14 is "IOC order was partially canceled due to incompletely filled" —
    // an expiry, not somebody's cancel.
    EXPECT_EQ(order.cancel_source, "14");
}

TEST(OkxWire, PendingOrderShipsAnEmptyFillTimeRatherThanNone) {
    const auto frame = fixture("rest_orders_pending.json");
    OrderBuffer storage{};
    const auto orders = parse_orders(frame, storage);
    ASSERT_TRUE(orders.has_value());
    ASSERT_EQ(orders->size(), 1U);

    const auto& order = orders->front();
    EXPECT_EQ(order.state, WireOrderState::Live);
    EXPECT_EQ(order.ord_type, "post_only");
    EXPECT_EQ(order.px, "13013.5");
    EXPECT_EQ(order.sz, "0.001");
    EXPECT_EQ(order.acc_fill_sz, "0");
    EXPECT_EQ(order.trade_id, "");
    // "" rather than an absent key: a reader that demands digits here refuses
    // every resting order.
    EXPECT_EQ(order.fill_ms, 0U);
    EXPECT_EQ(order.u_ms, 1'724'733'617'998ULL);
}

TEST(OkxWire, FillsAndPositionsParse) {
    const auto fills_body = fixture("rest_fills.json");
    FillBuffer fills_storage{};
    const auto fills = parse_fills(fills_body, fills_storage);
    ASSERT_TRUE(fills.has_value());
    ASSERT_EQ(fills->size(), 1U);
    EXPECT_EQ(fills->front().inst_id, "BTC-USDT");
    EXPECT_EQ(fills->front().trade_id, "744876980");
    EXPECT_EQ(fills->front().fill_px, "51858");
    EXPECT_EQ(fills->front().fill_sz, "0.00192834");
    EXPECT_EQ(fills->front().fee, "-0.00000192834");
    EXPECT_EQ(fills->front().exec_type, "T");
    EXPECT_EQ(fills->front().fill_ms, 1'708'587'373'361ULL);

    const auto positions_body = fixture("rest_positions.json");
    PositionBuffer position_storage{};
    const auto positions = parse_positions(positions_body, position_storage);
    ASSERT_TRUE(positions.has_value());
    ASSERT_EQ(positions->size(), 1U);
    EXPECT_EQ(positions->front().inst_id, "BTC-USDT");
    EXPECT_EQ(positions->front().pos, "0.00190433573");
    EXPECT_EQ(positions->front().pos_side, "net");
    EXPECT_EQ(positions->front().avg_px, "62961.4");
    EXPECT_EQ(positions->front().u_ms, 1'724'742'632'153ULL);
}

TEST(OkxWire, ARefusedOrderKeepsItsReasonInScode) {
    // The envelope says only "the operation failed". Classifying on that turns
    // insufficient balance, a duplicate id and a post-only that would have
    // crossed into one indistinguishable Terminal.
    const auto ok = fixture("rest_order_ack.json");
    auto envelope = parse_rest_envelope(ok);
    ASSERT_TRUE(envelope.has_value());
    EXPECT_EQ(envelope->code, k_ok);
    AckBuffer storage{};
    auto acks = parse_order_acks(ok, storage);
    ASSERT_TRUE(acks.has_value());
    ASSERT_EQ(acks->size(), 1U);
    EXPECT_EQ(acks->front().ord_id, "312269865356374016");
    EXPECT_EQ(acks->front().s_code, k_ok);

    const auto bad = fixture("rest_order_reject.json");
    envelope = parse_rest_envelope(bad);
    ASSERT_TRUE(envelope.has_value());
    EXPECT_EQ(envelope->code, k_operation_failed);
    acks = parse_order_acks(bad, storage);
    ASSERT_TRUE(acks.has_value());
    ASSERT_EQ(acks->size(), 1U);
    EXPECT_EQ(acks->front().s_code, k_insufficient_balance);
    EXPECT_EQ(acks->front().cl_ord_id, "01ARZ3NDEKTSV4RRFFQ69G5FAV");
    EXPECT_FALSE(acks->front().s_msg.empty());
}

TEST(OkxWire, PrivateParsersRefuseJunkRatherThanInventing) {
    auto orders = std::make_unique<OrderBuffer>();
    auto fills = std::make_unique<FillBuffer>();
    auto positions = std::make_unique<PositionBuffer>();
    auto acks = std::make_unique<AckBuffer>();
    for (const auto* junk : {"",
                             "{",
                             "[]",
                             "null",
                             "pong",
                             R"({"data":{}})",
                             R"({"arg":{"channel":"orders"}})",
                             R"({"data":[{"ordId":"1"}]})"}) {
        const std::string_view frame{junk};
        EXPECT_FALSE(parse_orders(frame, *orders).has_value()) << junk;
        EXPECT_FALSE(parse_fills(frame, *fills).has_value()) << junk;
        EXPECT_FALSE(parse_positions(frame, *positions).has_value()) << junk;
        EXPECT_FALSE(parse_order_acks(frame, *acks).has_value()) << junk;
    }
}

// ------------------------------------------------------------- requests ----

constexpr std::string_view k_api_key = "TESTKEY123";
constexpr std::string_view k_secret = "testsecret";
constexpr std::string_view k_passphrase = "testpass";
constexpr UnixNanos k_now{1'700'000'000'123'000'000ULL}; // 2023-11-14T22:13:20.123Z
constexpr std::string_view k_ulid = "01ARZ3NDEKTSV4RRFFQ69G5FAV";

exec::InstrumentTable order_entry_table() {
    exec::InstrumentTable table;
    WireInstrument swap{};
    swap.inst_id = "BTC-USDT-SWAP";
    swap.inst_type = "SWAP";
    swap.state = "live";
    swap.tick_size = "0.1";
    swap.lot_size = "0.01";
    swap.ct_val = "0.01";
    swap.ct_mult = "1";
    swap.ct_type = "linear";
    const auto perp = map_instrument(swap, MarketType::Perp);
    EXPECT_TRUE(perp.has_value());
    EXPECT_TRUE(table.add(*perp).has_value());

    WireInstrument spot{};
    spot.inst_id = "BTC-USDT";
    spot.inst_type = "SPOT";
    spot.state = "live";
    spot.tick_size = "0.1";
    spot.lot_size = "0.00000001";
    const auto cash = map_instrument(spot, MarketType::Spot);
    EXPECT_TRUE(cash.has_value());
    EXPECT_TRUE(table.add(*cash).has_value());
    return table;
}

void load_credentials(net::Signer& signer) {
    signer.set_api_key(k_api_key);
    ASSERT_TRUE(signer.secret().assign(k_secret).has_value());
}

std::string_view header(const net::RestRequest& request, std::string_view name) {
    for (const auto& h : request.headers) {
        if (h.name == name) {
            return h.value;
        }
    }
    return {};
}

OrderCmd limit_order(const exec::InstrumentTable& table, std::string_view symbol) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.instrument = base_of(*table.find(symbol)).id;
    cmd.side = Side::Buy;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.price = PriceTicks::from_raw(640'435); // 64043.5 at a 0.1 tick
    cmd.qty = QtyLots::from_raw(300);          // 3 contracts at a 0.01 lot
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from(k_ulid);
    return cmd;
}

TEST(OkxRest, TimestampIsIso8601MillisecondsInUtc) {
    // Epoch milliseconds are refused with 50112, and a local-time rendering
    // fails with 50102 — the same symptom as a clock an hour out of sync.
    std::array<char, 24> buf{};
    EXPECT_EQ(iso8601_ms(k_now, buf), "2023-11-14T22:13:20.123Z");
    EXPECT_EQ(iso8601_ms(UnixNanos{0}, buf), "1970-01-01T00:00:00.000Z");
    // A leap day, because the civil-date inversion is where that goes wrong.
    EXPECT_EQ(iso8601_ms(UnixNanos{1'709'164'800'000'000'000ULL}, buf),
              "2024-02-29T00:00:00.000Z");
}

TEST(OkxRest, GetSignsTheTargetIncludingItsQuestionMark) {
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder builder{signer};
    builder.set_passphrase(k_passphrase);

    const auto request = builder.open_orders(MarketType::Perp, k_now);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());
    EXPECT_EQ(request->method, net::HttpMethod::Get);
    EXPECT_EQ(request->target, "/api/v5/trade/orders-pending?instType=SWAP");
    EXPECT_TRUE(request->body.empty());

    // Where bybit signs the bare query, OKX signs the whole request path.
    EXPECT_EQ(builder.last_sign_message(),
              "2023-11-14T22:13:20.123ZGET/api/v5/trade/orders-pending?instType=SWAP");
    EXPECT_EQ(header(*request, "OK-ACCESS-SIGN"), "nTXtPpBhja/wts2kTr/a/B7UsuIEb9EPxI1Dis0AdfQ=");
    EXPECT_EQ(header(*request, "OK-ACCESS-KEY"), k_api_key);
    EXPECT_EQ(header(*request, "OK-ACCESS-TIMESTAMP"), "2023-11-14T22:13:20.123Z");
    EXPECT_EQ(header(*request, "OK-ACCESS-PASSPHRASE"), k_passphrase);
    EXPECT_TRUE(header(*request, "x-simulated-trading").empty());
}

TEST(OkxRest, PostSignsTimestampMethodPathAndBody) {
    const auto table = order_entry_table();
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder builder{signer};
    builder.set_passphrase(k_passphrase);
    builder.set_simulated(true);

    const auto request = builder.create_order(limit_order(table, "BTC-USDT-SWAP"), table, k_now);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());
    EXPECT_EQ(request->target, "/api/v5/trade/order");

    const std::string expected =
        std::string{"2023-11-14T22:13:20.123Z"} + "POST" + "/api/v5/trade/order" +
        std::string{request->body};
    EXPECT_EQ(builder.last_sign_message(), expected);

    std::array<char, net::k_base64_bytes> reference{};
    EXPECT_EQ(header(*request, "OK-ACCESS-SIGN"),
              net::hmac_sha256_base64(k_secret, expected, reference));
    // Demo trading is a header on the live host, not a different host.
    EXPECT_EQ(header(*request, "x-simulated-trading"), "1");
}

TEST(OkxRest, NoPassphraseIsNotSignedAtAll) {
    // A key and secret without the passphrase produce 50104 on every request,
    // which reads like a bad secret. Refusing to build says what is missing.
    const auto table = order_entry_table();
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder builder{signer};

    const auto request = builder.create_order(limit_order(table, "BTC-USDT-SWAP"), table, k_now);
    ASSERT_FALSE(request.has_value());
    EXPECT_EQ(request.error(), BuildError::NotSigned);
}

TEST(OkxRest, SpotAndSwapDifferInTradeModeAndSizing) {
    const auto table = order_entry_table();
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder builder{signer};
    builder.set_passphrase(k_passphrase);

    const auto swap = builder.create_order(limit_order(table, "BTC-USDT-SWAP"), table, k_now);
    ASSERT_TRUE(swap.has_value()) << to_string(swap.error());
    EXPECT_EQ(swap->body,
              R"({"instId":"BTC-USDT-SWAP","tdMode":"cross",)"
              R"("clOrdId":"01ARZ3NDEKTSV4RRFFQ69G5FAV","side":"buy","ordType":"limit",)"
              R"("sz":"3","px":"64043.5"})");

    auto cmd = limit_order(table, "BTC-USDT");
    cmd.qty = QtyLots::from_raw(300'000); // 0.003 BTC at a 1e-8 lot
    const auto spot = builder.create_order(cmd, table, k_now);
    ASSERT_TRUE(spot.has_value()) << to_string(spot.error());
    EXPECT_EQ(spot->body,
              R"({"instId":"BTC-USDT","tdMode":"cash",)"
              R"("clOrdId":"01ARZ3NDEKTSV4RRFFQ69G5FAV","side":"buy","ordType":"limit",)"
              R"("sz":"0.003","px":"64043.5"})");
}

TEST(OkxRest, SpotMarketBuyIsSizedInTheBaseCurrency) {
    // Without tgtCcy, OKX reads sz on a spot market BUY as QUOTE currency, so
    // "buy 0.003 BTC" places an order to spend 0.003 USDT.
    const auto table = order_entry_table();
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder builder{signer};
    builder.set_passphrase(k_passphrase);

    auto cmd = limit_order(table, "BTC-USDT");
    cmd.order_type = OrderType::Market;
    cmd.qty = QtyLots::from_raw(300'000);
    const auto request = builder.create_order(cmd, table, k_now);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());
    EXPECT_NE(request->body.find(R"("tgtCcy":"base_ccy")"), std::string_view::npos)
        << request->body;
    EXPECT_EQ(request->body.find(R"("px")"), std::string_view::npos) << "market carries no price";

    // A swap market order must NOT carry it: tgtCcy is spot-only.
    auto perp = limit_order(table, "BTC-USDT-SWAP");
    perp.order_type = OrderType::Market;
    const auto swap = builder.create_order(perp, table, k_now);
    ASSERT_TRUE(swap.has_value());
    EXPECT_EQ(swap->body.find("tgtCcy"), std::string_view::npos) << swap->body;
}

TEST(OkxRest, PostOnlyAndTimeInForceAreOrderTypes) {
    // OKX has no timeInForce field. A post-only flag sent beside ordType
    // "limit" is accepted and the order crosses.
    const auto table = order_entry_table();
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder builder{signer};
    builder.set_passphrase(k_passphrase);

    auto cmd = limit_order(table, "BTC-USDT-SWAP");
    cmd.flags = OrderFlags::PostOnly;
    auto request = builder.create_order(cmd, table, k_now);
    ASSERT_TRUE(request.has_value());
    EXPECT_NE(request->body.find(R"("ordType":"post_only")"), std::string_view::npos);
    EXPECT_EQ(request->body.find("postOnly"), std::string_view::npos);

    cmd.flags = OrderFlags::None;
    cmd.tif = TimeInForce::Ioc;
    request = builder.create_order(cmd, table, k_now);
    ASSERT_TRUE(request.has_value());
    EXPECT_NE(request->body.find(R"("ordType":"ioc")"), std::string_view::npos);

    cmd.tif = TimeInForce::Fok;
    request = builder.create_order(cmd, table, k_now);
    ASSERT_TRUE(request.has_value());
    EXPECT_NE(request->body.find(R"("ordType":"fok")"), std::string_view::npos);

    cmd.tif = TimeInForce::Gtc;
    cmd.flags = OrderFlags::ReduceOnly;
    request = builder.create_order(cmd, table, k_now);
    ASSERT_TRUE(request.has_value());
    EXPECT_NE(request->body.find(R"("reduceOnly":true)"), std::string_view::npos);
}

TEST(OkxRest, EveryMarketIsAnsweredAndOptionsRefuseByName) {
    EXPECT_EQ(inst_type_of(MarketType::Spot), "SPOT");
    EXPECT_EQ(inst_type_of(MarketType::Perp), "SWAP");
    EXPECT_EQ(inst_type_of(MarketType::Future), "FUTURES");
    EXPECT_EQ(inst_type_of(MarketType::Option), "OPTION");

    EXPECT_EQ(td_mode_of(MarketType::Spot).value(), "cash");
    EXPECT_EQ(td_mode_of(MarketType::Perp).value(), "cross");
    EXPECT_EQ(td_mode_of(MarketType::Future).value(), "cross");

    // OKX lists options, and a price for one is a premium in the underlying
    // coin with three mutually exclusive spellings. Refusing by name beats
    // sending it down the swap path.
    const auto option = td_mode_of(MarketType::Option);
    ASSERT_FALSE(option.has_value());
    EXPECT_EQ(option.error(), BuildError::UnsupportedMarket);
}

TEST(OkxRest, CancelAndAmendTargetTheOrderByWhicheverIdIsPresent) {
    const auto table = order_entry_table();
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder builder{signer};
    builder.set_passphrase(k_passphrase);

    auto cmd = limit_order(table, "BTC-USDT-SWAP");
    cmd.kind = CmdKind::Cancel;
    auto request = builder.cancel_order(cmd, table, k_now);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->target, "/api/v5/trade/cancel-order");
    EXPECT_EQ(request->body,
              R"({"instId":"BTC-USDT-SWAP","clOrdId":"01ARZ3NDEKTSV4RRFFQ69G5FAV"})");

    cmd.target_exchange_order_id = *ExchangeOrderId::from("680800019749904385");
    request = builder.cancel_order(cmd, table, k_now);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->body, R"({"instId":"BTC-USDT-SWAP","ordId":"680800019749904385"})");

    // newSz is the order's new TOTAL size, not the remainder.
    cmd.kind = CmdKind::Amend;
    cmd.qty = QtyLots::from_raw(800);
    cmd.price = PriceTicks::undef();
    request = builder.amend_order(cmd, table, k_now);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->target, "/api/v5/trade/amend-order");
    EXPECT_EQ(request->body,
              R"({"instId":"BTC-USDT-SWAP","ordId":"680800019749904385","newSz":"8"})");
}

TEST(OkxRest, CancelBatchIsAJsonArrayCappedAtTwenty) {
    const auto table = order_entry_table();
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder builder{signer};
    builder.set_passphrase(k_passphrase);

    std::vector<OrderCmd> cmds;
    for (int i = 0; i < 25; ++i) {
        auto cmd = limit_order(table, "BTC-USDT-SWAP");
        cmd.kind = CmdKind::Cancel;
        cmds.push_back(cmd);
    }
    const auto first = builder.cancel_batch(cmds, table, k_now);
    ASSERT_TRUE(first.has_value()) << to_string(first.error());
    EXPECT_EQ(first->consumed, 20U); // the venue rejects an oversized frame whole
    EXPECT_EQ(first->request.target, "/api/v5/trade/cancel-batch-orders");
    EXPECT_EQ(first->request.body.front(), '[');
    EXPECT_EQ(first->request.body.back(), ']');

    const auto second =
        builder.cancel_batch(std::span<const OrderCmd>{cmds}.subspan(20), table, k_now);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->consumed, 5U);

    EXPECT_FALSE(builder.cancel_batch({}, table, k_now).has_value());
}

TEST(OkxRest, PullsNameTheInstrumentTypeAndCarryASince) {
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder builder{signer};
    builder.set_passphrase(k_passphrase);

    const auto fills = builder.fills(MarketType::Spot, UnixNanos{1'700'000'000'000'000'000ULL},
                                     k_now);
    ASSERT_TRUE(fills.has_value());
    EXPECT_EQ(fills->target, "/api/v5/trade/fills?instType=SPOT&begin=1700000000000");

    const auto all = builder.fills(MarketType::Perp, UnixNanos{0}, k_now);
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all->target, "/api/v5/trade/fills?instType=SWAP");

    const auto positions = builder.positions(MarketType::Perp, k_now);
    ASSERT_TRUE(positions.has_value());
    EXPECT_EQ(positions->target, "/api/v5/account/positions?instType=SWAP");
}

TEST(OkxWs, LoginSignsEpochSecondsAndTheVerifyPath) {
    // Nothing about this resembles the REST signature: seconds rather than
    // ISO-8601, and a path no request ever goes to.
    net::Signer signer;
    load_credentials(signer);
    Protocol protocol;

    const auto frame = protocol.login_frame(signer, k_passphrase, k_now);
    ASSERT_TRUE(frame.has_value()) << to_string(frame.error());
    EXPECT_EQ(protocol.last_login_message(), "1700000000GET/users/self/verify");
    EXPECT_EQ(*frame,
              R"({"op":"login","args":[{"apiKey":"TESTKEY123","passphrase":"testpass",)"
              R"("timestamp":"1700000000",)"
              R"("sign":"nbqmCslY7ltfyiVOn8iY7qsU13F+ycpPNKvAVTNSMVc="}]})");

    Protocol unsigned_protocol;
    net::Signer empty;
    EXPECT_FALSE(unsigned_protocol.login_frame(empty, k_passphrase, k_now).has_value());
    EXPECT_FALSE(unsigned_protocol.login_frame(signer, {}, k_now).has_value());
}

TEST(OkxWs, PrivateSubscribeNamesAnInstrumentTypeAndPublicNamesAnInstrument) {
    Protocol protocol;
    const std::array<Subscription, 2> private_args{
        Subscription{.channel = k_orders_channel, .inst_type = "SPOT"},
        Subscription{.channel = k_orders_channel, .inst_type = "SWAP"}};
    const auto orders = protocol.subscribe_frame(private_args);
    ASSERT_TRUE(orders.has_value());
    EXPECT_EQ(orders->consumed, 2U);
    EXPECT_EQ(orders->text,
              R"({"op":"subscribe","args":[{"channel":"orders","instType":"SPOT"},)"
              R"({"channel":"orders","instType":"SWAP"}]})");

    // The public shape is unchanged: one instId, no instType.
    const std::array<Subscription, 1> public_args{
        Subscription{.channel = "bbo-tbt", .inst_id = "BTC-USDT-SWAP"}};
    const auto bbo = protocol.subscribe_frame(public_args);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->text,
              R"({"op":"subscribe","args":[{"channel":"bbo-tbt","instId":"BTC-USDT-SWAP"}]})");

    // Neither key set is a subscription to everything; OKX answers 60018.
    const std::array<Subscription, 1> empty{Subscription{.channel = "orders"}};
    EXPECT_FALSE(protocol.subscribe_frame(empty).has_value());
}

} // namespace
} // namespace raikiri::venues::okx
