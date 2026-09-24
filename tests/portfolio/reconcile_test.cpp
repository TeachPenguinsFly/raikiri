#include <raikiri/portfolio/reconcile.hpp>

#include <gtest/gtest.h>

namespace raikiri::portfolio {
namespace {

constexpr auto k_instrument = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{1});

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
    const auto holding = reconcile_position(false, std::nullopt, QtyLots::from_raw(10));
    EXPECT_EQ(holding.verdict, ReconVerdict::Unknown);

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
    const auto out = reconcile_position(true, reported(-5), QtyLots::from_raw(0));
    EXPECT_EQ(out.verdict, ReconVerdict::Break);
    EXPECT_EQ(out.detail, "venue holds a position we do not know about");
}

TEST(Reconcile, SignsMustMatchNotJustMagnitudes) {
    const auto out = reconcile_position(true, reported(-10), QtyLots::from_raw(10));
    EXPECT_EQ(out.verdict, ReconVerdict::Break);
}

TEST(Reconcile, TheVenuesAverageEntryIsRecordedNeverAdopted) {
    const auto out = reconcile_position(true, reported(10), QtyLots::from_raw(10));
    EXPECT_EQ(out.verdict, ReconVerdict::Agree);
    EXPECT_EQ(out.venue_qty.raw, 10);
}

} // namespace
} // namespace raikiri::portfolio
