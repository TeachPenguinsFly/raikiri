#include <raikiri/domain/identifiers.hpp>
#include <raikiri/journal/reader.hpp>
#include <raikiri/journal/writer.hpp>
#include <raikiri/persist/mapper.hpp>
#include <raikiri/persist/sink.hpp>
#include <raikiri/schema/codec.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <unistd.h>

namespace raikiri::persist {
namespace {

using msgs = raikiri_journal::schema::messages;
namespace jt = raikiri_journal::types;

std::string fresh_dir(const char* name) {
    const auto dir = "/tmp/raikiri-persist-test-" + std::to_string(::getpid()) + "-" + name;
    (void)std::system(("rm -rf " + dir).c_str());
    return dir;
}

// Writes one full-fill OrderEvent + one funding + one BBO into a journal and
// returns the frames via a reader.
std::string build_journal(const char* name) {
    const auto dir = fresh_dir(name);
    auto writer = journal::JournalWriter::open(dir, "journal");
    EXPECT_TRUE(writer.has_value());

    (void)schema::append_msg<msgs::OrderEventMsg>(*writer, 100, [](auto m) {
        m.eventType(jt::orderEventTypeEnum::Filled);
        m.side(jt::sideEnum::Buy);
        m.orderType(jt::orderTypeEnum::Limit);
        m.tif(jt::tifEnum::Gtc);
        m.flags(0);
        m.liquidity(jt::liquidityEnum::Maker);
        m.instrument(jt::instrumentId{
            InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{7}).packed});
        m.clientOrderIdField(jt::clientOrderId{ClientOrderId::make(3, 42).raw});
        m.exchangeClientOrderIdField().assign_string("01ARZ3NDEKTSV4RRFFQ69G5FAV");
        m.exchangeOrderIdField().assign_string("EX-123");
        m.execIdField().assign_string("FILL-9");
        m.price(jt::priceTicks{500'000});
        m.qty(jt::qtyLots{25});
        m.leavesQty(jt::qtyLots{0});
        m.fee(jt::fixedE9{1'000'000});
        m.tsEvent(jt::timestampNs{100});
        m.tsInit(jt::timestampNs{101});
    });

    (void)schema::append_msg<msgs::FundingMsg>(*writer, 200, [](auto m) {
        m.instrument(jt::instrumentId{
            InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{7}).packed});
        m.rate(jt::fixedE9{100'000});
        m.predictedRate(jt::fixedE9{110'000});
        m.markPrice(jt::priceTicks{500'100});
        m.nextFundingTime(jt::timestampNs{999});
        m.intervalS(28'800);
        m.tsEvent(jt::timestampNs{200});
        m.tsInit(jt::timestampNs{201});
    });

    (void)schema::append_msg<msgs::BboMsg>(*writer, 300, [](auto m) {
        m.instrument(jt::instrumentId{1});
        m.bid(jt::priceTicks{1});
        m.ask(jt::priceTicks{2});
        m.bidQty(jt::qtyLots{1});
        m.askQty(jt::qtyLots{1});
        m.tsEvent(jt::timestampNs{300});
        m.tsInit(jt::timestampNs{301});
    });
    return dir;
}

TEST(Mapper, FillProducesEventUpsertAndExecution) {
    const auto dir = build_journal("fill");
    auto reader = journal::JournalReader::open(dir, "journal");
    ASSERT_TRUE(reader.has_value());

    RowBatch batch;
    journal::FrameView frame{};
    while (reader->next(frame) == journal::ReadResult::Item) {
        map_frame(frame, batch);
    }

    ASSERT_EQ(batch.order_events.size(), 1U);
    ASSERT_EQ(batch.order_upserts.size(), 1U);
    ASSERT_EQ(batch.executions.size(), 1U);
    ASSERT_EQ(batch.funding.size(), 1U);
    EXPECT_EQ(batch.last_seq, 3U); // BBO advanced the cursor without rows

    const auto& ev = batch.order_events[0];
    EXPECT_EQ(ev.event_type, "filled");
    EXPECT_EQ(ev.exec_id, "FILL-9");
    EXPECT_EQ(ev.fee_e9, 1'000'000);

    const auto& up = batch.order_upserts[0];
    EXPECT_EQ(up.status, "filled"); // leaves==0 -> full fill
    EXPECT_EQ(up.exchange_client_order_id, "01ARZ3NDEKTSV4RRFFQ69G5FAV");
    EXPECT_EQ(up.exchange_order_id, "EX-123");
    EXPECT_EQ(up.side, "buy");

    const auto& ex = batch.executions[0];
    EXPECT_EQ(ex.venue, "bybit");
    EXPECT_EQ(ex.exec_id, "FILL-9");
    EXPECT_EQ(ex.liquidity, "maker");
    EXPECT_EQ(ex.client_order_id, ClientOrderId::make(3, 42).raw);
}

TEST(Mapper, PartialFillKeepsPartialStatusAndAckHasNoExecution) {
    const auto dir = fresh_dir("partial");
    auto writer = journal::JournalWriter::open(dir, "journal");
    ASSERT_TRUE(writer.has_value());

    (void)schema::append_msg<msgs::OrderEventMsg>(*writer, 1, [](auto m) {
        m.eventType(jt::orderEventTypeEnum::Accepted);
        m.instrument(jt::instrumentId{
            InstrumentId::make(VenueId::Sim, MarketType::Perp, SymbolId{1}).packed});
        m.clientOrderIdField(jt::clientOrderId{1});
        m.qty(jt::qtyLots{10});
        m.leavesQty(jt::qtyLots{10});
        m.tsEvent(jt::timestampNs{1});
        m.tsInit(jt::timestampNs{1});
    });
    (void)schema::append_msg<msgs::OrderEventMsg>(*writer, 2, [](auto m) {
        m.eventType(jt::orderEventTypeEnum::Filled);
        m.instrument(jt::instrumentId{
            InstrumentId::make(VenueId::Sim, MarketType::Perp, SymbolId{1}).packed});
        m.clientOrderIdField(jt::clientOrderId{1});
        m.execIdField().assign_string("E1");
        m.qty(jt::qtyLots{4});
        m.leavesQty(jt::qtyLots{6});
        m.tsEvent(jt::timestampNs{2});
        m.tsInit(jt::timestampNs{2});
    });

    auto reader = journal::JournalReader::open(dir, "journal");
    ASSERT_TRUE(reader.has_value());
    RowBatch batch;
    journal::FrameView frame{};
    while (reader->next(frame) == journal::ReadResult::Item) {
        map_frame(frame, batch);
    }

    ASSERT_EQ(batch.order_events.size(), 2U);
    ASSERT_EQ(batch.executions.size(), 1U); // only the fill, not the ack
    EXPECT_EQ(batch.order_upserts[0].status, "accepted");
    EXPECT_EQ(batch.order_upserts[1].status, "partially_filled"); // leaves > 0
}

// Full pipeline against a live Postgres — skipped unless RAIKIRI_PG_DSN is set
// (CI provides a service container; locally: infra postgres or docker).
// The golden invariant: persisting the same journal twice changes nothing.
TEST(PersistIntegration, ReplayTwiceIsIdempotent) {
    const char* dsn = std::getenv("RAIKIRI_PG_DSN");
    if (dsn == nullptr || *dsn == '\0') {
        GTEST_SKIP() << "RAIKIRI_PG_DSN not set";
    }
    const auto dir = build_journal("pg");

    PgSink sink{dsn};
    sink.exec_file(std::string{RAIKIRI_SQL_DIR} + "/migrations/0001_init.sql");

    const auto run_once = [&] {
        auto reader = journal::JournalReader::open_after(dir, "journal",
                                                         sink.load_cursor("test-journal"));
        EXPECT_TRUE(reader.has_value());
        RowBatch batch;
        journal::FrameView frame{};
        while (reader->next(frame) == journal::ReadResult::Item) {
            map_frame(frame, batch);
        }
        if (batch.last_seq != 0) {
            sink.commit("test-journal", batch);
        }
    };

    run_once();
    const auto cursor_after_first = sink.load_cursor("test-journal");
    EXPECT_EQ(cursor_after_first, 3U);

    // Reset the cursor to force a full re-read, then persist again: every
    // insert must be absorbed by ON CONFLICT.
    RowBatch reset;
    reset.last_seq = 0;
    sink.commit("test-journal", reset); // cursor -> 0
    run_once();
    EXPECT_EQ(sink.load_cursor("test-journal"), 3U);
}

} // namespace
} // namespace raikiri::persist
