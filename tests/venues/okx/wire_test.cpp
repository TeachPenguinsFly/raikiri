#include <raikiri/venues/okx/wire.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

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

} // namespace
} // namespace raikiri::venues::okx
