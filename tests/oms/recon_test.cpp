#include <raikiri/oms/recon.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace raikiri::oms {
namespace {

constexpr InstrumentId k_btc = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{1});
constexpr InstrumentId k_eth = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{2});

TEST(BookAsOf, AFillTheVenueHadNotSeenIsRewoundBackOut) {
    const std::vector<BookedFill> booked{{.ts_ns = 1'500, .lots = 3}};
    EXPECT_EQ(book_as_of(10, booked, 1'000), 7);
}

TEST(BookAsOf, AFillTheVenueHADSeenIsLeftAlone) {
    const std::vector<BookedFill> booked{{.ts_ns = 900, .lots = 3}};
    EXPECT_EQ(book_as_of(10, booked, 1'000), 10);
}

TEST(BookAsOf, AFillStampedExactlyAtTheBoundaryCountsAsSeen) {
    const std::vector<BookedFill> booked{{.ts_ns = 1'000, .lots = 3}};
    EXPECT_EQ(book_as_of(10, booked, 1'000), 10);
}

TEST(BookAsOf, SellsRewindTheOtherWay) {
    const std::vector<BookedFill> booked{{.ts_ns = 1'500, .lots = -4}};
    EXPECT_EQ(book_as_of(6, booked, 1'000), 10);
}

TEST(BookAsOf, ManyFillsOnBothSidesOfTheBoundary) {
    const std::vector<BookedFill> booked{
        {.ts_ns = 800, .lots = 5},    // seen
        {.ts_ns = 1'200, .lots = 3},  // not seen
        {.ts_ns = 1'300, .lots = -1}, // not seen
        {.ts_ns = 999, .lots = 2},    // seen
    };
    EXPECT_EQ(book_as_of(9, booked, 1'000), 7);
}

TEST(PruneFills, DropsWhatNoSnapshotCanReachBackOver) {
    std::vector<BookedFill> booked{{.ts_ns = 1'000, .lots = 1}, {.ts_ns = 2'000, .lots = 2}};
    prune_fills(booked, UnixNanos{10'000}, 5'000);
    EXPECT_TRUE(booked.empty()) << "past the window they can never be rewound past again";
}

TEST(PruneFills, KeepsWhatAStillValidSnapshotCouldRewind) {
    std::vector<BookedFill> booked{{.ts_ns = 1'000, .lots = 1}, {.ts_ns = 9'000, .lots = 2}};
    prune_fills(booked, UnixNanos{10'000}, 5'000);
    ASSERT_EQ(booked.size(), 1U);
    EXPECT_EQ(booked.front().ts_ns, 9'000U);
}

TEST(PruneFills, AClockBeforeTheWindowDropsNothing) {
    std::vector<BookedFill> booked{{.ts_ns = 10, .lots = 1}};
    prune_fills(booked, UnixNanos{100}, 5'000);
    EXPECT_EQ(booked.size(), 1U) << "no underflow: the cutoff floors at zero";
}

TEST(ReconGate, OneDisagreementIsNotABreak) {
    ReconGate gate;
    EXPECT_FALSE(gate.on_break(k_btc, 1'000));
    EXPECT_FALSE(gate.halted(k_btc));
}

TEST(ReconGate, ADisagreementAtASecondSnapshotIsOne) {
    ReconGate gate;
    EXPECT_FALSE(gate.on_break(k_btc, 1'000));
    EXPECT_TRUE(gate.on_break(k_btc, 2'000)) << "an independent pull said the same thing";
    EXPECT_TRUE(gate.halted(k_btc));
}

TEST(ReconGate, TheSameSnapshotReadTwiceConfirmsNothing) {
    ReconGate gate;
    EXPECT_FALSE(gate.on_break(k_btc, 1'000));
    EXPECT_FALSE(gate.on_break(k_btc, 1'000));
    EXPECT_FALSE(gate.halted(k_btc));
}

TEST(ReconGate, AgreementForgetsTheFirstDisagreementButNotTheHalt) {
    ReconGate gate;
    EXPECT_FALSE(gate.on_break(k_btc, 1'000));
    gate.on_agree(k_btc);
    EXPECT_FALSE(gate.on_break(k_btc, 2'000));

    EXPECT_TRUE(gate.on_break(k_btc, 3'000));
    ASSERT_TRUE(gate.halted(k_btc));
    gate.on_agree(k_btc);
    EXPECT_TRUE(gate.halted(k_btc)) << "a halt is unwound by a person, not by the next lucky pull";
}

TEST(ReconGate, InstrumentsAreHeldApart) {
    ReconGate gate;
    EXPECT_FALSE(gate.on_break(k_btc, 1'000));
    EXPECT_FALSE(gate.on_break(k_eth, 2'000)) << "eth's first, not eth's second";
    EXPECT_TRUE(gate.on_break(k_btc, 2'000));
    EXPECT_TRUE(gate.halted(k_btc));
    EXPECT_FALSE(gate.halted(k_eth));
    EXPECT_EQ(gate.halted_count(), 1U);
}

} // namespace
} // namespace raikiri::oms
