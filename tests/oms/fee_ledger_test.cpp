#include <raikiri/oms/fee_ledger.hpp>

#include <gtest/gtest.h>

namespace raikiri::oms {
namespace {

constexpr InstrumentId k_instrument = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{1});

LedgerEntry charge(std::string_view reference, std::int64_t amount_e9) {
    LedgerEntry out{};
    out.instrument = k_instrument;
    out.kind = LedgerKind::Fee;
    out.amount = FixedE9::from_raw(amount_e9);
    out.reference = *ExecId::from(reference);
    return out;
}

TEST(FeeLedger, TheTwoFiguresAgreeingIsTheEvidenceTheFillWasReadRight) {
    FeeLedger ledger;
    ledger.book("exec-1", FixedE9::from_raw(-7'236'500), UnixNanos{1'000});

    const auto checked = ledger.reconcile(charge("exec-1", -7'236'500));
    EXPECT_EQ(checked.verdict, FeeVerdict::Agreed);
    EXPECT_EQ(checked.booked.raw, -7'236'500);
}

TEST(FeeLedger, ADifferenceIsABreakAndCarriesBothFiguresToSayItWith) {
    FeeLedger ledger;
    ledger.book("exec-2", FixedE9::from_raw(-7'236'500), UnixNanos{1'000});

    const auto checked = ledger.reconcile(charge("exec-2", -7'236'999));
    EXPECT_EQ(checked.verdict, FeeVerdict::Disagreed);
    EXPECT_EQ(checked.booked.raw, -7'236'500);
}

TEST(FeeLedger, AChargeAgainstNoFillWeHoldIsNotAZeroWeClaimed) {
    FeeLedger ledger;
    const auto checked = ledger.reconcile(charge("exec-nobody", -1'000));
    EXPECT_EQ(checked.verdict, FeeVerdict::Unmatched);
    EXPECT_TRUE(checked.booked.is_undef());
}

TEST(FeeLedger, NothingWorthQuotingBackIsNotRemembered) {
    FeeLedger ledger;
    ledger.book("", FixedE9::from_raw(-500), UnixNanos{1'000});
    ledger.book("exec-3", FixedE9::from_raw(0), UnixNanos{1'000});
    EXPECT_EQ(ledger.size(), 0U) << "an empty reference cannot be joined on and a zero fee cannot disagree";
}

TEST(FeeLedger, PruningDropsOnlyWhatTheVenueWillNeverQuoteAgain) {
    FeeLedger ledger;
    ledger.book("old", FixedE9::from_raw(-1), UnixNanos{1'000});
    ledger.book("fresh", FixedE9::from_raw(-2), UnixNanos{9'000});

    ledger.prune(UnixNanos{10'000}, 5'000);
    EXPECT_EQ(ledger.size(), 1U);
    EXPECT_EQ(ledger.reconcile(charge("old", -1)).verdict, FeeVerdict::Unmatched);
    EXPECT_EQ(ledger.reconcile(charge("fresh", -2)).verdict, FeeVerdict::Agreed);
}

TEST(FeeLedger, ARebookedFeeReplacesRatherThanAccumulates) {
    FeeLedger ledger;
    ledger.book("exec-4", FixedE9::from_raw(-100), UnixNanos{1'000});
    ledger.book("exec-4", FixedE9::from_raw(-250), UnixNanos{2'000});
    EXPECT_EQ(ledger.size(), 1U);
    EXPECT_EQ(ledger.reconcile(charge("exec-4", -250)).verdict, FeeVerdict::Agreed);
}

} // namespace
} // namespace raikiri::oms
