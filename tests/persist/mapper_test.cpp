#include <raikiri/domain/identifiers.hpp>
#include <raikiri/journal/reader.hpp>
#include <raikiri/journal/writer.hpp>
#include <raikiri/oms/funding_identity.hpp>
#include <raikiri/persist/mapper.hpp>
#include <raikiri/persist/sink.hpp>
#include <raikiri/schema/codec.hpp>

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdlib>
#include <string>

namespace raikiri::persist {
namespace {

using msgs = raikiri_journal::schema::messages;
namespace jt = raikiri_journal::types;

// A directory of this test's own, removed when the test leaves it.
//
// CLEANED AT BOTH ENDS. The pid is in the name so `ctest -j` can run these in
// parallel, which also means the wipe below never matches a PREVIOUS run's
// directory — so every run left its journal behind. A hundred and ten of them
// had accumulated in /tmp, 370 MB, where nobody would think to look.
class FreshDir {
public:
    explicit FreshDir(const char* name)
        : path_{"/tmp/raikiri-persist-test-" + std::to_string(::getpid()) + "-" + name} {
        wipe();
    }

    ~FreshDir() { wipe(); }

    FreshDir(const FreshDir&) = delete;
    FreshDir& operator=(const FreshDir&) = delete;

    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    operator const std::string&() const noexcept { return path_; } // NOLINT(google-explicit-constructor)

private:
    void wipe() const { (void)std::system(("rm -rf " + path_).c_str()); }

    std::string path_;
};

// Writes one full-fill OrderEvent + one funding + one BBO into a journal and
// returns the frames via a reader.
// TAKES the directory rather than making one: the caller uses the path after
// this returns, so the guard that wipes it has to outlive the call.
std::string build_journal(const std::string& dir) {
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

    // TWO STRATEGIES, ONE SETTLEMENT. This is the pair whose keys used to
    // collide when a long symbol truncated the identity: same instrument, same
    // settlement second, different books. If the key cannot tell them apart,
    // ON CONFLICT DO NOTHING keeps one and the other's money is never booked.
    // A SETTLEMENT NOBODY HAS PERSISTED YET. The key is deterministic by
    // design, so a fixed stamp would already be in the table on the second
    // execution against the same database and the first pass would insert
    // nothing — which is indistinguishable from the row being dropped.
    const auto settled = UnixNanos{1'788'696'006'115'000'000ULL +
                                   static_cast<std::uint64_t>(::getpid()) * 1'000'000'000ULL};
    for (const std::uint16_t strategy : {std::uint16_t{3}, std::uint16_t{10}}) {
        (void)schema::append_msg<msgs::PnlIncrementMsg>(
            *writer, 400 + strategy, [strategy, settled](auto m) {
                const auto instrument =
                    InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{7});
                std::array<char, oms::k_max_funding_identity> buf{};
                m.kind(jt::pnlKindEnum::Funding);
                m.settleSide(jt::settleSideEnum::Quote);
                m.strategyId(strategy);
                m.instrument(jt::instrumentId{instrument.packed});
                m.symbol().assign_range(std::string_view{"BTCUSDT"});
                m.clientOrderIdField(jt::clientOrderId{0});
                m.execIdField().assign_range(
                    oms::funding_identity(instrument, settled, strategy, buf));
                m.amount(jt::fixedE9{-250'000});
                m.tsEvent(jt::timestampNs{settled.ns});
            });
    }

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
    const FreshDir scratch{"fill"};
    const auto dir = build_journal(scratch);
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
    // Two of the five frames are the funding increments the idempotency test
    // needs; they map to pnl_increments and not to anything asserted above.
    ASSERT_EQ(batch.pnl_increments.size(), 2U);
    EXPECT_EQ(batch.last_seq, 5U); // the BBO advanced the cursor without rows

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
    const FreshDir dir{"partial"};
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

// The ledger's idempotency key. `pnl_increments` dedupes on source_id with ON
// CONFLICT DO NOTHING, so a key that repeats is money silently discarded and a
// key that changes between replays is money booked twice. Nothing asserted it
// until this test.
TEST(Mapper, TheLedgerKeyNamesTheKindTheVenueAndTheFill) {
    const FreshDir dir{"pnl"};
    auto writer = journal::JournalWriter::open(dir, "journal");
    ASSERT_TRUE(writer.has_value());

    const auto instrument =
        InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{7}).packed;
    const auto write = [&](jt::pnlKindEnum kind, std::string_view exec_id, std::int64_t amount) {
        (void)schema::append_msg<msgs::PnlIncrementMsg>(*writer, 1, [&](auto m) {
            m.kind(kind);
            m.settleSide(jt::settleSideEnum::Quote);
            m.strategyId(3);
            m.instrument(jt::instrumentId{instrument});
            m.symbol().assign_string("BTCUSDT");
            m.clientOrderIdField(jt::clientOrderId{ClientOrderId::make(3, 42).raw});
            m.execIdField().assign_range(exec_id);
            m.amount(jt::fixedE9{amount});
            m.tsEvent(jt::timestampNs{100});
        });
    };
    write(jt::pnlKindEnum::Realized, "FILL-9", 5'000'000);
    write(jt::pnlKindEnum::Fee, "FILL-9", -1'000'000);
    write(jt::pnlKindEnum::Realized, "FILL-10", 2'000'000);

    auto reader = journal::JournalReader::open(dir, "journal");
    ASSERT_TRUE(reader.has_value());
    RowBatch batch;
    journal::FrameView frame{};
    while (reader->next(frame) == journal::ReadResult::Item) {
        map_frame(frame, batch);
    }

    ASSERT_EQ(batch.pnl_increments.size(), 3U);
    EXPECT_EQ(batch.pnl_increments[0].source_id, "realized:bybit:FILL-9");
    // Two kinds of one fill are two increments, not one: they are separate
    // money and the key has to say which.
    EXPECT_EQ(batch.pnl_increments[1].source_id, "fee:bybit:FILL-9");
    EXPECT_EQ(batch.pnl_increments[2].source_id, "realized:bybit:FILL-10");

    // A fee is money leaving, and the ledger's stated sign is "positive is
    // money gained". The writer negates it; nothing downstream may put it back.
    EXPECT_LT(batch.pnl_increments[1].amount_e9, 0);
}

// Full pipeline against a live Postgres — skipped unless RAIKIRI_PG_DSN is set
// (CI provides a service container; locally: infra postgres or docker).
// The golden invariant: persisting the same journal twice changes nothing.
TEST(PersistIntegration, ReplayTwiceIsIdempotent) {
    const char* dsn = std::getenv("RAIKIRI_PG_DSN");
    if (dsn == nullptr || *dsn == '\0') {
        GTEST_SKIP() << "RAIKIRI_PG_DSN not set";
    }
    const FreshDir scratch{"pg"};
    const auto dir = build_journal(scratch);

    PgSink sink{dsn};
    for (const auto* migration : {"/migrations/0001_init.sql", "/migrations/0002_positions.sql"}) {
        sink.exec_file(std::string{RAIKIRI_SQL_DIR} + migration);
    }

    const auto run_once = [&] {
        auto reader =
            journal::JournalReader::open_after(dir, "journal", sink.load_cursor("test-journal"));
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

    // THE CURSOR OUTLIVES THE JOURNAL. It is keyed by name in the database and
    // the journal directory is new every run, so a cursor left at 3 by an
    // earlier execution makes the first pass start midway and read fewer
    // frames than the second — which reads as a duplicate and is not one.
    // Both passes are given the same starting point deliberately.
    const auto rewind = [&] {
        RowBatch reset;
        reset.last_seq = 0;
        sink.commit("test-journal", reset);
    };
    // Counted rather than assumed empty: this runs against a database that
    // holds whatever earlier work left in it, and the invariant is the DELTA.
    const auto increments_before = sink.count_rows("pnl_increments");

    rewind();
    run_once();
    EXPECT_EQ(sink.load_cursor("test-journal"), 5U);
    const auto increments = sink.count_rows("pnl_increments");
    const auto events = sink.count_rows("order_events");
    // Both funding rows landed: the two strategies sharing one settlement are
    // the pair a truncated key used to fold into one.
    EXPECT_GE(increments - increments_before, 2)
        << "one strategy's funding never reached the ledger";

    rewind();
    run_once();
    EXPECT_EQ(sink.load_cursor("test-journal"), 5U);
    // THE CURSOR ALONE PROVES NOTHING: a replay that duplicated every row
    // advances it to the same number. The counts are the invariant.
    EXPECT_EQ(sink.count_rows("pnl_increments"), increments);
    EXPECT_EQ(sink.count_rows("order_events"), events);
}

} // namespace

TEST(Mapper, PositionRowsCarryCostRatherThanAnAverage) {
    // An average is cost over size and can be recomputed whenever it is asked
    // for. Storing it instead would fix it to the size it was computed at, and
    // every later partial close would drift away from it.
    const FreshDir dir{"position"};
    auto writer = journal::JournalWriter::open(dir, "journal");
    ASSERT_TRUE(writer.has_value());

    (void)schema::append_msg<msgs::PositionMsg>(*writer, 500, [](auto m) {
        m.strategyId(7);
        m.settleSide(jt::settleSideEnum::Quote);
        m.venue(jt::venueIdEnum::Bybit);
        m.instrument(jt::instrumentId{42});
        m.symbol().assign_range(std::string_view{"BTCUSDT"});
        m.netQty(jt::qtyLots{3});
        m.cost(jt::fixedE9{180'000'000'000});
        m.realized(jt::fixedE9{-500});
        m.fees(jt::fixedE9{25});
        m.fillCount(4);
        m.tsOpened(jt::timestampNs{100});
        m.tsUpdated(jt::timestampNs{500});
    });

    auto reader = journal::JournalReader::open(dir, "journal");
    ASSERT_TRUE(reader.has_value());
    RowBatch batch;
    journal::FrameView frame{};
    while (reader->next(frame) == journal::ReadResult::Item) {
        map_frame(frame, batch);
    }

    ASSERT_EQ(batch.positions.size(), 1U);
    const auto& row = batch.positions[0];
    EXPECT_EQ(row.strategy_id, 7);
    EXPECT_EQ(row.venue, "bybit");
    EXPECT_EQ(row.exchange_symbol, "BTCUSDT");
    EXPECT_EQ(row.instrument_id, 42U);
    EXPECT_EQ(row.net_qty_lots, 3);
    EXPECT_EQ(row.cost_e9, 180'000'000'000);
    EXPECT_EQ(row.realized_e9, -500);
    EXPECT_EQ(row.fees_e9, 25);
    EXPECT_EQ(row.settle_side, "quote");
    EXPECT_EQ(row.fill_count, 4U);
    EXPECT_EQ(row.ts_opened, 100U);
    EXPECT_EQ(row.ts_updated, 500U);
}

} // namespace raikiri::persist
