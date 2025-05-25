#include <raikiri/exec/reconcile.hpp>

#include <gtest/gtest.h>

namespace raikiri::exec {
namespace {

constexpr auto k_instrument =
    InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{1});

PositionReport reported(std::int64_t lots) {
    PositionReport report{};
    report.instrument = k_instrument;
    report.net_qty = QtyLots::from_raw(lots);
    report.avg_entry = PriceTicks::from_raw(999'999); // deliberately absurd
    return report;
}

TEST(Reconcile, AgreesWhenBothSidesSayTheSame) {
    const auto out = reconcile_position(true, reported(10), QtyLots::from_raw(10));
    EXPECT_EQ(out.verdict, ReconVerdict::Agree);

    const auto flat = reconcile_position(true, std::nullopt, QtyLots::from_raw(0));
    EXPECT_EQ(flat.verdict, ReconVerdict::Agree);
}

TEST(Reconcile, AVenueThatCannotAnswerIsNeverReadAsFlat) {
    // The whole point of the caps row. Fourteen of sixteen adapters cannot
    // perform this pull, and reading their silence as "flat" would break every
    // position we hold on them.
    const auto holding = reconcile_position(false, std::nullopt, QtyLots::from_raw(10));
    EXPECT_EQ(holding.verdict, ReconVerdict::Unknown);

    // Unknown even when we happen to be flat: the answer is about the venue's
    // ability to answer, not about the number.
    const auto flat = reconcile_position(false, std::nullopt, QtyLots::from_raw(0));
    EXPECT_EQ(flat.verdict, ReconVerdict::Unknown);
}

TEST(Reconcile, VenueSayingFlatWhileWeHoldSomethingIsABreak) {
    const auto out = reconcile_position(true, std::nullopt, QtyLots::from_raw(10));
    EXPECT_EQ(out.verdict, ReconVerdict::Break);
    EXPECT_EQ(out.venue_qty.raw, 0);
    EXPECT_EQ(out.local_qty.raw, 10);
}

TEST(Reconcile, APositionWeDoNotKnowAboutIsTheDangerousBreak) {
    // Historically an orphan from a previous process generation: it is on the
    // venue, it is losing or making money, and nothing here is watching it.
    const auto out = reconcile_position(true, reported(-5), QtyLots::from_raw(0));
    EXPECT_EQ(out.verdict, ReconVerdict::Break);
    EXPECT_EQ(out.detail, "venue holds a position we do not know about");
}

TEST(Reconcile, SignsMustMatchNotJustMagnitudes) {
    // Long ten against short ten is the worst possible disagreement and the
    // easiest to miss by comparing absolute values.
    const auto out = reconcile_position(true, reported(-10), QtyLots::from_raw(10));
    EXPECT_EQ(out.verdict, ReconVerdict::Break);
}

TEST(Reconcile, TheVenuesAverageEntryIsRecordedNeverAdopted) {
    // reconcile_position returns quantities only. There is nowhere in the
    // result to put a cost basis, which is what stops one being imported: the
    // venue is authoritative on whether a position exists, never on what it
    // cost.
    const auto out = reconcile_position(true, reported(10), QtyLots::from_raw(10));
    EXPECT_EQ(out.verdict, ReconVerdict::Agree);
    EXPECT_EQ(out.venue_qty.raw, 10);
}

} // namespace
} // namespace raikiri::exec
