#include <raikiri/shm/records.hpp>

#include <gtest/gtest.h>

#include <memory>

namespace raikiri::shm {
namespace {

constexpr auto k_btc = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{7});
constexpr std::uint64_t k_pull_ns = 1'000'000'000;

// The channel is 256 KiB of seqlock cells; the tests only need the type.
std::unique_ptr<PositionTable> fresh_table() {
    return std::make_unique<PositionTable>();
}

TEST(PositionTable, AnUnwrittenCellSaysNothingRatherThanFlat) {
    const auto table = fresh_table();
    PositionCell cell{};
    // read() returning 0 is the seqlock saying "never written". A caller that
    // ignored it would read a zeroed struct and conclude the venue reports
    // flat, which is the one wrong answer this whole channel exists to avoid.
    EXPECT_EQ(table->read(k_btc.symbol().raw, cell), 0U);
}

TEST(PositionTable, AWrittenZeroIsTheVenueSayingFlat) {
    const auto table = fresh_table();
    PositionCell cell{};
    cell.report.instrument = k_btc;
    // Spelled out, because a default-constructed QtyLots is `undef` and not
    // zero: "flat" is a value the writer states, never one it leaves behind.
    cell.report.net_qty = QtyLots::from_raw(0);
    cell.ts_pulled = UnixNanos{k_pull_ns};
    table->write(k_btc.symbol().raw, cell);

    PositionCell out{};
    ASSERT_NE(table->read(k_btc.symbol().raw, out), 0U);
    EXPECT_EQ(out.report.net_qty.raw, 0);
    // The distinguishing fact is the timestamp, not the quantity: this is a
    // fresh zero, and freshness is what separates it from the case above.
    EXPECT_EQ(out.ts_pulled.ns, k_pull_ns);
}

TEST(PositionTable, TheRowSurvivesTheRoundTripIntact) {
    const auto table = fresh_table();
    PositionCell cell{};
    cell.report.instrument = k_btc;
    cell.report.net_qty = QtyLots::from_raw(-250);
    cell.report.avg_entry = PriceTicks::from_raw(64'000'00);
    cell.report.ts_venue = UnixNanos{k_pull_ns - 5};
    cell.ts_pulled = UnixNanos{k_pull_ns};
    table->write(k_btc.symbol().raw, cell);

    PositionCell out{};
    ASSERT_NE(table->read(k_btc.symbol().raw, out), 0U);
    EXPECT_EQ(out.report.instrument, k_btc);
    EXPECT_EQ(out.report.net_qty.raw, -250); // sign carries short, not magnitude
    EXPECT_EQ(out.report.avg_entry.raw, 64'000'00);
    EXPECT_EQ(out.report.ts_venue.ns, k_pull_ns - 5);
}

TEST(PositionTable, AZeroedCellIsUndefinedAndNotFlat) {
    // The trap this channel sits on: value-initialising the POD does NOT
    // produce a flat position, it produces INT64_MIN. Anything that publishes
    // a row must say what the row means.
    const PositionCell cell{};
    EXPECT_TRUE(cell.report.net_qty.is_undef());
    EXPECT_NE(cell.report.net_qty.raw, 0);
}

TEST(PositionTable, EveryCatalogueSlotHasACellToWriteTo) {
    // A SymbolId indexes both tables. A positions table smaller than the
    // catalogue would mint ids with no cell, and those instruments would be
    // permanently unreconcilable while looking no different from the rest.
    EXPECT_EQ(k_position_cells, k_catalogue_cells);
}

} // namespace
} // namespace raikiri::shm
