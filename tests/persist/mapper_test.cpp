#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/origin.hpp>
#include <raikiri/journal/reader.hpp>
#include <raikiri/journal/writer.hpp>
#include <raikiri/oms/funding_identity.hpp>
#include <raikiri/persist/mapper.hpp>
#include <raikiri/persist/sink.hpp>
#include <raikiri/schema/codec.hpp>

#include <gtest/gtest.h>

#include <unistd.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <pqxx/pqxx>
#include <span>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

namespace raikiri::persist {
namespace {

using msgs = raikiri_journal::schema::messages;
namespace jt = raikiri_journal::types;

// a per-test directory wiped on entry and exit
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
    void wipe() const {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    std::string path_;
};

// writes one fill, one funding and one bbo; the caller owns the directory
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
        m.instrument(jt::instrumentId{InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{7}).packed});
        m.clientOrderIdField(jt::clientOrderId{ClientOrderId::make(3, 42).raw});
        m.exchangeClientOrderIdField().assign_string("01ARZ3NDEKTSV4RRFFQ69G5FAV");
        m.exchangeOrderIdField().assign_string("EX-123");
        m.execIdField().assign_string("FILL-9");
        m.price(jt::priceTicks{500'000});
        m.qty(jt::qtyLots{25});
        m.leavesQty(jt::qtyLots{0});
        m.fee(jt::fixedE9{1'000'000});
        m.feeAsset().assign_range(std::string_view{"BTC"});
        m.tsEvent(jt::timestampNs{100});
        m.tsInit(jt::timestampNs{101});
    });

    (void)schema::append_msg<msgs::FundingMsg>(*writer, 200, [](auto m) {
        m.instrument(jt::instrumentId{InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{7}).packed});
        m.rate(jt::fixedE9{100'000});
        m.predictedRate(jt::fixedE9{110'000});
        m.markPrice(jt::priceTicks{500'100});
        m.nextFundingTime(jt::timestampNs{999});
        m.intervalS(28'800);
        m.accrual(jt::fundingAccrualEnum::Continuous);
        m.tsEvent(jt::timestampNs{200});
        m.tsInit(jt::timestampNs{201});
    });

    // two strategies settling one instrument in the same second; a fresh stamp so reruns insert rows
    const auto settled = UnixNanos{std::uint64_t{1'788'696'006'115'000'000} +
                                   static_cast<std::uint64_t>(::getpid()) * std::uint64_t{1'000'000'000}};
    for (const std::uint16_t strategy : {std::uint16_t{3}, std::uint16_t{10}}) {
        (void)schema::append_msg<msgs::PnlIncrementMsg>(*writer, 400 + strategy, [strategy, settled](auto m) {
            const auto instrument = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{7});
            std::array<char, oms::k_max_funding_identity> buf{};
            m.kind(jt::pnlKindEnum::Funding);
            m.settleSide(jt::settleSideEnum::Quote);
            m.strategyId(strategy);
            m.instrument(jt::instrumentId{instrument.packed});
            m.symbol().assign_range(std::string_view{"BTCUSDT"});
            m.clientOrderIdField(jt::clientOrderId{0});
            m.execIdField().assign_range(oms::funding_identity(instrument, settled, strategy, buf));
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

void append_instrument(journal::JournalWriter& writer,
                       InstrumentId id,
                       std::uint64_t generation,
                       std::string_view symbol,
                       std::int64_t tick_e9,
                       std::uint64_t loaded_at) {
    (void)schema::append_msg<msgs::InstrumentMsg>(writer, loaded_at, [&](auto m) {
        m.instrument(jt::instrumentId{id.packed});
        m.generation(generation);
        m.symbol().assign_range(symbol);
        m.localSymbol().assign_range(std::string_view{"BTC/USDT"});
        m.kind(jt::contractKindEnum::Inverse);
        m.tickSize(jt::fixedE9{tick_e9});
        m.lotSize(jt::fixedE9{1'000'000});
        m.minNotional(jt::fixedE9{5'000'000'000});
        m.contractValue(jt::fixedE9{0});
        m.tsLoaded(jt::timestampNs{loaded_at});
        m.assetClass(jt::assetClassEnum::Equity);
        m.underlying().assign_range(std::string_view{"TSLA"});
    });
}

TEST(Mapper, ACatalogueEntryRoundTripsWithItsGeneration) {
    const FreshDir dir{"instrument"};
    const auto id = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{7});
    {
        auto writer = journal::JournalWriter::open(dir, "journal");
        ASSERT_TRUE(writer.has_value());
        append_instrument(*writer, id, 77, "BTCUSD", 500'000'000, 1'000);
        append_instrument(*writer, id, 78, "ZEROUSD", 0, 2'000);
    }
    auto reader = journal::JournalReader::open(dir, "journal");
    ASSERT_TRUE(reader.has_value());
    RowBatch batch;
    journal::FrameView frame{};
    while (reader->next(frame) == journal::ReadResult::Item) {
        map_frame(frame, batch);
    }

    ASSERT_EQ(batch.instruments.size(), 1U) << "a zero tick is refused by the table, so dropped";
    EXPECT_EQ(batch.last_seq, 2U);
    const auto& row = batch.instruments[0];
    EXPECT_EQ(row.instrument_id, id.packed);
    EXPECT_EQ(row.generation, 77U);
    EXPECT_EQ(row.venue, "bybit");
    EXPECT_EQ(row.market_type, to_string(MarketType::Perp));
    EXPECT_EQ(row.exchange_symbol, "BTCUSD");
    EXPECT_EQ(row.local_symbol, "BTC/USDT");
    EXPECT_EQ(row.contract_kind, "inverse");
    EXPECT_EQ(row.asset_class, "equity");
    EXPECT_EQ(row.underlying, "TSLA");
    EXPECT_EQ(row.tick_size_e9, 500'000'000);
    EXPECT_EQ(row.lot_size_e9, 1'000'000);
    EXPECT_EQ(row.min_notional_e9, 5'000'000'000);
    EXPECT_EQ(row.contract_value_e9, 1'000'000'000);
    EXPECT_EQ(row.valid_from, 1'000U);
}

TEST(Mapper, ACreateCommandCarriesItsOrigin) {
    const FreshDir dir{"origin"};
    const auto child = ClientOrderId::make_child(1030, 5);
    {
        auto writer = journal::JournalWriter::open(dir, "journal");
        ASSERT_TRUE(writer.has_value());
        (void)schema::append_msg<msgs::OrderCmdMsg>(*writer, 1, [&](auto m) {
            m.kind(jt::cmdKindEnum::Create);
            m.clientOrderIdField(jt::clientOrderId{child.raw});
            m.tsCreated(jt::timestampNs{77});
            m.bookKind(static_cast<std::uint8_t>(BookKind::Hedge));
            m.parentId(jt::clientOrderId{9000});
            m.externalRef().assign_range(std::string_view{"rfq-12"});
        });
        (void)schema::append_msg<msgs::OrderCmdMsg>(*writer, 2, [&](auto m) {
            m.kind(jt::cmdKindEnum::Cancel);
            m.clientOrderIdField(jt::clientOrderId{child.raw});
            m.tsCreated(jt::timestampNs{78});
        });
    }
    auto reader = journal::JournalReader::open(dir, "journal");
    ASSERT_TRUE(reader.has_value());
    RowBatch batch;
    journal::FrameView frame{};
    while (reader->next(frame) == journal::ReadResult::Item) {
        map_frame(frame, batch);
    }
    ASSERT_EQ(batch.order_origins.size(), 1U) << "a cancel says nothing about whose the order is";
    const auto& row = batch.order_origins[0];
    EXPECT_EQ(row.client_order_id, child.raw);
    EXPECT_EQ(row.book_id, 1030);
    EXPECT_EQ(row.book_kind, "hedge");
    EXPECT_EQ(row.parent_id, 9000U);
    EXPECT_EQ(row.external_ref, "rfq-12");
    EXPECT_EQ(row.ts_created, 77U);
    EXPECT_EQ(batch.last_seq, 2U);
}

// journals written before schema version 7 stay on disk: such a record is shorter, and reading its origin would run off
// the end. the frame sits in an exact heap block here, so asan catches any read past it
TEST(Mapper, AnOldCommandRecordHasNoOriginAndIsNotReadPastItsEnd) {
    constexpr std::size_t k_origin_bytes = 1 + 8 + 40;
    using Msg = sbepp::message_traits<msgs::OrderCmdMsg>::value_type<std::byte>;
    std::array<std::byte, 512> buf{};
    Msg msg{buf.data(), buf.size()};
    sbepp::fill_message_header(msg);
    msg.kind(jt::cmdKindEnum::Create);
    msg.clientOrderIdField(jt::clientOrderId{ClientOrderId::make(5, 1).raw});
    msg.tsCreated(jt::timestampNs{1});
    const auto used = static_cast<std::size_t>(sbepp::size_bytes(msg)) - k_origin_bytes;
    std::vector<std::byte> shorter(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(used));

    const FreshDir dir{"old-origin"};
    {
        auto writer = journal::JournalWriter::open(dir, "journal");
        ASSERT_TRUE(writer.has_value());
        ASSERT_TRUE(writer->append(schema::message_id<msgs::OrderCmdMsg>(), 6, std::span<const std::byte>{shorter}, 1)
                        .has_value());
    }
    auto reader = journal::JournalReader::open(dir, "journal");
    ASSERT_TRUE(reader.has_value());
    journal::FrameView frame{};
    ASSERT_EQ(reader->next(frame), journal::ReadResult::Item);
    EXPECT_EQ(frame.schema_version, 6U);
    ASSERT_EQ(frame.payload.size(), used);

    frame.payload = std::span<const std::byte>{shorter};
    RowBatch batch;
    map_frame(frame, batch);
    EXPECT_TRUE(batch.order_origins.empty());
    EXPECT_EQ(batch.last_seq, 1U) << "the record still advances the cursor";
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
    ASSERT_EQ(batch.pnl_increments.size(), 2U);
    EXPECT_EQ(batch.last_seq, 5U); // the BBO advanced the cursor without rows
    EXPECT_EQ(batch.funding[0].accrual, std::optional<std::string>{"continuous"});

    const auto& ev = batch.order_events[0];
    EXPECT_EQ(ev.event_type, "filled");
    EXPECT_EQ(ev.exec_id, "FILL-9");
    EXPECT_EQ(ev.fee_e9, 1'000'000);
    EXPECT_EQ(ev.fee_asset, "BTC");

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
        m.instrument(jt::instrumentId{InstrumentId::make(VenueId::Sim, MarketType::Perp, SymbolId{1}).packed});
        m.clientOrderIdField(jt::clientOrderId{1});
        m.qty(jt::qtyLots{10});
        m.leavesQty(jt::qtyLots{10});
        m.tsEvent(jt::timestampNs{1});
        m.tsInit(jt::timestampNs{1});
    });
    (void)schema::append_msg<msgs::OrderEventMsg>(*writer, 2, [](auto m) {
        m.eventType(jt::orderEventTypeEnum::Filled);
        m.instrument(jt::instrumentId{InstrumentId::make(VenueId::Sim, MarketType::Perp, SymbolId{1}).packed});
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

// pnl_increments dedupes on source_id with on conflict do nothing
TEST(Mapper, TheLedgerKeyNamesTheKindTheVenueAndTheFill) {
    const FreshDir dir{"pnl"};
    auto writer = journal::JournalWriter::open(dir, "journal");
    ASSERT_TRUE(writer.has_value());

    const auto instrument = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{7}).packed;
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
    EXPECT_EQ(batch.pnl_increments[1].source_id, "fee:bybit:FILL-9");
    EXPECT_EQ(batch.pnl_increments[2].source_id, "realized:bybit:FILL-10");

    // fees are negated: positive means money gained
    EXPECT_LT(batch.pnl_increments[1].amount_e9, 0);
}

void migrate(PgSink& sink) {
    for (const auto* migration : {"/migrations/0001_init.sql",
                                  "/migrations/0002_positions.sql",
                                  "/migrations/0003_reject_reason.sql",
                                  "/migrations/0004_funding_accrual.sql",
                                  "/migrations/0005_instrument_generations.sql",
                                  "/migrations/0006_instrument_asset_class.sql",
                                  "/migrations/0007_fee_asset.sql",
                                  "/migrations/0008_order_origins.sql",
                                  "/migrations/0009_order_version.sql"}) {
        sink.exec_file(std::string{RAIKIRI_SQL_DIR} + migration);
    }
}

TEST(PersistIntegration, AGenerationClosesThePreviousWindowOnItsId) {
    const char* dsn = std::getenv("RAIKIRI_PG_DSN");
    if (dsn == nullptr || *dsn == '\0') {
        GTEST_SKIP() << "RAIKIRI_PG_DSN not set";
    }
    PgSink sink{dsn};
    migrate(sink);
    migrate(sink);

    const auto id = InstrumentId::make(VenueId::Sim, MarketType::Perp, SymbolId{0xFFFF'FFF2U});
    const auto key = std::to_string(static_cast<std::int64_t>(id.packed));
    pqxx::connection conn{dsn};
    const auto scrub = [&] {
        pqxx::work tx{conn};
        tx.exec("DELETE FROM raikiri.instruments WHERE instrument_id = " + key);
        tx.commit();
    };
    scrub();

    const FreshDir dir{"generations"};
    {
        auto writer = journal::JournalWriter::open(dir, "journal");
        ASSERT_TRUE(writer.has_value());
        append_instrument(*writer, id, 1, "OLDUSD", 10'000'000, 1'000);
        append_instrument(*writer, id, 2, "NEWUSD", 500'000'000, 2'000);
    }
    const auto replay = [&] {
        auto reader = journal::JournalReader::open(dir, "journal");
        ASSERT_TRUE(reader.has_value());
        RowBatch batch;
        journal::FrameView frame{};
        while (reader->next(frame) == journal::ReadResult::Item) {
            map_frame(frame, batch);
        }
        sink.commit("test-generations", batch);
    };
    replay();
    replay();

    pqxx::work tx{conn};
    EXPECT_EQ(tx.query_value<std::string>("SELECT string_agg(generation || ':' || exchange_symbol || ':' ||"
                                          " valid_from || '-' || COALESCE(valid_to::text, ''), ','"
                                          " ORDER BY generation) FROM raikiri.instruments"
                                          " WHERE instrument_id = " +
                                          key),
              "1:OLDUSD:1000-2000,2:NEWUSD:2000-");
    tx.commit();
    scrub();
}

// events race on the wire: every arrival order must leave the row the sent order leaves
TEST(PersistIntegration, AnOrderRowConvergesInAnyArrivalOrder) {
    const char* dsn = std::getenv("RAIKIRI_PG_DSN");
    if (dsn == nullptr || *dsn == '\0') {
        GTEST_SKIP() << "RAIKIRI_PG_DSN not set";
    }
    PgSink sink{dsn};
    migrate(sink);
    pqxx::connection conn{dsn};

    const auto row = [](std::string status, std::int64_t leaves, std::uint64_t ts) {
        OrderUpsert out{};
        out.client_order_id = 0x0FFF'0000'0000'0000ULL;
        out.instrument_id = 1;
        out.side = "buy";
        out.order_type = "limit";
        out.tif = "gtc";
        out.price_ticks = 100;
        out.qty_lots = 10;
        out.leaves_lots = leaves;
        out.status = std::move(status);
        out.ts_event = ts;
        return out;
    };
    const auto read = [&] {
        pqxx::read_transaction tx{conn};
        const auto rows = tx.exec("SELECT status, leaves_lots, price_ticks, version FROM raikiri.orders"
                                  " WHERE client_order_id = " +
                                  std::to_string(static_cast<std::int64_t>(0x0FFF'0000'0000'0000ULL)));
        const auto got = rows[0];
        return std::tuple{
            got[0].as<std::string>(), got[1].as<std::int64_t>(), got[2].as<std::int64_t>(), got[3].as<std::int64_t>()};
    };
    const auto play = [&](std::vector<OrderUpsert> rows) {
        {
            pqxx::work tx{conn};
            tx.exec("DELETE FROM raikiri.orders WHERE client_order_id = " +
                    std::to_string(static_cast<std::int64_t>(0x0FFF'0000'0000'0000ULL)));
            tx.commit();
        }
        RowBatch batch{};
        batch.order_upserts = std::move(rows);
        sink.commit("order-race-test", batch);
    };
    auto amend = row("", 10, 30);
    amend.price_ticks = 90;
    amend.reprices = true;
    constexpr std::int64_t k_undef = std::numeric_limits<std::int64_t>::min();

    // a venue's rest ack stamped after the fill it raced
    play({row("submitted", 10, 1), row("filled", 0, 50), row("accepted", 10, 60)});
    EXPECT_EQ(std::get<0>(read()), "filled");
    EXPECT_EQ(std::get<1>(read()), 0);

    // a fill stamped before the cancel it raced, seen after it
    play({row("submitted", 10, 1), row("canceled", 10, 60), row("partially_filled", 7, 50)});
    EXPECT_EQ(std::get<0>(read()), "canceled");
    EXPECT_EQ(std::get<1>(read()), 7);

    // a fill that finishes an order someone had already cancelled
    play({row("submitted", 10, 1), row("canceled", 10, 60), row("filled", 0, 50)});
    EXPECT_EQ(std::get<0>(read()), "filled");

    // fills overtaking each other, and a pulled fill with no leaves at all
    play({row("submitted", 10, 1),
          row("partially_filled", 3, 50),
          row("partially_filled", 7, 40),
          row("partially_filled", k_undef, 70)});
    EXPECT_EQ(std::get<0>(read()), "partially_filled");
    EXPECT_EQ(std::get<1>(read()), 3);

    // an older working status stays out, an amend's price comes in, a fill's price does not
    auto priced_fill = row("partially_filled", 8, 40);
    priced_fill.price_ticks = 77;
    play({row("accepted", 10, 20), amend, priced_fill, row("accepted", 8, 10)});
    EXPECT_EQ(std::get<0>(read()), "partially_filled");
    EXPECT_EQ(std::get<2>(read()), 90);
    EXPECT_EQ(std::get<3>(read()), 3);
    {
        pqxx::work tx{conn};
        tx.exec("DELETE FROM raikiri.orders WHERE client_order_id = " +
                std::to_string(static_cast<std::int64_t>(0x0FFF'0000'0000'0000ULL)));
        tx.commit();
    }
}

// skipped unless RAIKIRI_PG_DSN is set
TEST(PersistIntegration, ReplayTwiceIsIdempotent) {
    const char* dsn = std::getenv("RAIKIRI_PG_DSN");
    if (dsn == nullptr || *dsn == '\0') {
        GTEST_SKIP() << "RAIKIRI_PG_DSN not set";
    }
    const FreshDir scratch{"pg"};
    const auto dir = build_journal(scratch);

    PgSink sink{dsn};
    migrate(sink);

    const auto run_once = [&] {
        auto reader = journal::JournalReader::open_after(dir, "journal", sink.load_cursor("test-journal"));
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

    // the cursor is keyed by name in the database, so rewind it before each pass
    const auto rewind = [&] {
        RowBatch reset;
        reset.last_seq = 0;
        sink.commit("test-journal", reset);
    };
    // the database keeps earlier runs' rows; assert the delta
    const auto increments_before = sink.count_rows("pnl_increments");

    rewind();
    run_once();
    EXPECT_EQ(sink.load_cursor("test-journal"), 5U);
    const auto increments = sink.count_rows("pnl_increments");
    const auto events = sink.count_rows("order_events");
    EXPECT_GE(increments - increments_before, 2) << "one strategy's funding never reached the ledger";

    rewind();
    run_once();
    EXPECT_EQ(sink.load_cursor("test-journal"), 5U);
    EXPECT_EQ(sink.count_rows("pnl_increments"), increments);
    EXPECT_EQ(sink.count_rows("order_events"), events);
}

} // namespace

TEST(Mapper, PositionRowsCarryCostRatherThanAnAverage) {
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
