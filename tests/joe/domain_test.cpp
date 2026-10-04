#include <raikiri/joe/parent.hpp>
#include <raikiri/joe/schedule.hpp>
#include <raikiri/joe/sor.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace raikiri::joe {
namespace {

constexpr std::uint64_t k_s = 1'000'000'000ULL;

FixedE9 d(std::string_view text) {
    return *parse_decimal_e9(text);
}

InstrumentId venue(VenueId id) {
    return InstrumentId::make(id, MarketType::Perp, SymbolId{7});
}

VenueBook book(VenueId id, std::string_view fee, std::string_view lot, std::vector<Level> levels) {
    return {.instrument = venue(id), .taker_fee = d(fee), .lot = d(lot), .levels = std::move(levels)};
}

const Leg* leg_on(const Route& route, VenueId id) {
    for (const auto& leg : route.legs) {
        if (leg.instrument == venue(id)) {
            return &leg;
        }
    }
    return nullptr;
}

TEST(JoeSor, PrefersTheCheaperVenueAfterFees) {
    // binance quotes the better raw price, but okx is cheaper once the taker fee is in
    const std::vector books{book(VenueId::Binance, "0.0005", "0.001", {{d("100.00"), d("1")}}),
                            book(VenueId::Okx, "0.0002", "0.001", {{d("100.02"), d("1")}})};
    const auto route = joe::route(Side::Buy, d("1.5"), FixedE9::undef(), books);
    ASSERT_EQ(route.legs.size(), 2U);
    EXPECT_EQ(leg_on(route, VenueId::Okx)->qty, d("1"));
    EXPECT_EQ(leg_on(route, VenueId::Binance)->qty, d("0.5"));
    EXPECT_EQ(leg_on(route, VenueId::Binance)->lots, 500);
    EXPECT_EQ(leg_on(route, VenueId::Okx)->fee, d("0.020004"));
    EXPECT_EQ(route.unrouted, d("0"));
}

TEST(JoeSor, WalksDepthAndSetsTheLimitAtTheDeepestLevel) {
    const std::vector books{
        book(VenueId::Binance, "0", "0.1", {{d("100"), d("1")}, {d("101"), d("1")}, {d("103"), d("5")}}),
        book(VenueId::Bybit, "0", "0.1", {{d("102"), d("1")}})};
    const auto route = joe::route(Side::Buy, d("3"), FixedE9::undef(), books);
    EXPECT_EQ(leg_on(route, VenueId::Binance)->qty, d("2"));
    EXPECT_EQ(leg_on(route, VenueId::Binance)->limit, d("101"));
    EXPECT_EQ(leg_on(route, VenueId::Binance)->value, d("201"));
    EXPECT_EQ(leg_on(route, VenueId::Bybit)->qty, d("1"));
    EXPECT_EQ(leg_on(route, VenueId::Bybit)->limit, d("102"));
}

TEST(JoeSor, StopsAtTheParentLimit) {
    const std::vector books{book(VenueId::Binance, "0", "0.1", {{d("100"), d("1")}, {d("105"), d("10")}})};
    const auto route = joe::route(Side::Buy, d("3"), d("104"), books);
    ASSERT_EQ(route.legs.size(), 1U);
    EXPECT_EQ(route.legs[0].qty, d("1"));
    EXPECT_EQ(route.unrouted, d("2"));
}

TEST(JoeSor, SellsToTheHighestBidAfterFees) {
    const std::vector books{book(VenueId::Gate, "0.00075", "0.01", {{d("100.05"), d("1")}}),
                            book(VenueId::Bybit, "0.00055", "0.01", {{d("100.03"), d("1")}})};
    const auto route = joe::route(Side::Sell, d("1"), FixedE9::undef(), books);
    ASSERT_EQ(route.legs.size(), 1U);
    EXPECT_EQ(route.legs[0].instrument, venue(VenueId::Bybit));
}

TEST(JoeLots, RoundDownToWholeLots) {
    EXPECT_EQ(round_down(d("0.0257"), d("0.001")), d("0.025"));
    EXPECT_EQ(lots_in(d("0.0257"), d("0.001")), 25);
    EXPECT_EQ(round_down(d("0.0009"), d("0.001")), d("0"));
    EXPECT_EQ(lots_in(d("1"), FixedE9::from_raw(0)), 0);
}

TEST(JoeLots, RemainderGoesToAVenueWithRoomAndTheRestStaysUnrouted) {
    // fine lots on a shallow cheap venue, coarse lots on a deep one
    const std::vector books{book(VenueId::Binance, "0", "0.01", {{d("100"), d("0.05")}}),
                            book(VenueId::Bybit, "0", "0.1", {{d("101"), d("1")}})};
    const auto exact = joe::route(Side::Buy, d("0.25"), FixedE9::undef(), books);
    EXPECT_EQ(leg_on(exact, VenueId::Binance)->qty, d("0.05"));
    EXPECT_EQ(leg_on(exact, VenueId::Bybit)->qty, d("0.2"));
    EXPECT_EQ(exact.unrouted, d("0"));

    const auto ragged = joe::route(Side::Buy, d("0.27"), FixedE9::undef(), books);
    EXPECT_EQ(leg_on(ragged, VenueId::Bybit)->lots, 2);
    EXPECT_EQ(ragged.unrouted, d("0.02"));
}

TEST(JoeLots, ALegUnderTheMinimumNotionalMovesToAnotherVenue) {
    auto small = book(VenueId::Okx, "0", "0.01", {{d("100"), d("0.03")}});
    small.min_notional = d("5");
    const std::vector books{small, book(VenueId::Bybit, "0", "0.01", {{d("100.5"), d("1")}})};
    const auto route = joe::route(Side::Buy, d("0.1"), FixedE9::undef(), books);
    EXPECT_EQ(leg_on(route, VenueId::Okx), nullptr);
    EXPECT_EQ(leg_on(route, VenueId::Bybit)->qty, d("0.1"));
    EXPECT_EQ(route.unrouted, d("0"));
}

TEST(JoeTwap, SlicesAreEvenWithoutJitter) {
    const auto times = slice_times(UnixNanos{1000 * k_s}, 600 * k_s, 10, 0, 1);
    ASSERT_EQ(times.size(), 10U);
    for (std::size_t i = 0; i < times.size(); ++i) {
        EXPECT_EQ(times[i].ns, (1000 + 60 * i) * k_s);
    }
    EXPECT_EQ(fired(times, UnixNanos{1000 * k_s}), 1U);
    EXPECT_EQ(fired(times, UnixNanos{1179 * k_s}), 3U);
    EXPECT_EQ(fired(times, UnixNanos{5000 * k_s}), 10U);
}

TEST(JoeTwap, JitterStaysInBoundsKeepsOrderAndRepeats) {
    const auto times = slice_times(UnixNanos{0}, 600 * k_s, 10, 2500, 42);
    EXPECT_EQ(times, slice_times(UnixNanos{0}, 600 * k_s, 10, 2500, 42));
    EXPECT_NE(times, slice_times(UnixNanos{0}, 600 * k_s, 10, 2500, 43));
    EXPECT_EQ(times[0].ns, 0U);
    for (std::size_t i = 1; i < times.size(); ++i) {
        const auto planned = 60 * k_s * i;
        EXPECT_GE(times[i].ns, planned - 15 * k_s);
        EXPECT_LE(times[i].ns, planned + 15 * k_s);
        EXPECT_GT(times[i], times[i - 1]);
    }
}

TEST(JoeTwap, CumulativeTargetEndsOnTheTotalExactly) {
    const auto weights = flat_weights(3);
    const auto total = d("1");
    EXPECT_EQ(cumulative_target(total, weights, 0), d("0"));
    EXPECT_EQ(cumulative_target(total, weights, 1), d("0.333333333"));
    EXPECT_EQ(cumulative_target(total, weights, 2), d("0.666666666"));
    EXPECT_EQ(cumulative_target(total, weights, 3), total);
    EXPECT_EQ(slice_size(total, weights, 2), d("0.333333334"));
}

TEST(JoeTwap, CatchUpIsCappedUntilTheLastSlice) {
    // three slices of 0.1 are due, only 0.1 filled: the shortfall is 0.2
    EXPECT_EQ(next_slice(d("0.3"), d("0.1"), d("0"), d("0.1"), 200, false), d("0.2"));
    EXPECT_EQ(next_slice(d("0.3"), d("0.1"), d("0"), d("0.1"), 150, false), d("0.15"));
    EXPECT_EQ(next_slice(d("0.3"), d("0.1"), d("0.05"), d("0.1"), 200, false), d("0.15"));
    EXPECT_EQ(next_slice(d("1"), d("0.1"), d("0"), d("0.1"), 150, true), d("0.9"));
    EXPECT_EQ(next_slice(d("0.3"), d("0.35"), d("0"), d("0.1"), 200, false), d("0"));
}

TEST(JoeVwap, TargetFollowsTheProfile) {
    const std::vector<std::uint64_t> weights{1, 2, 3, 4};
    EXPECT_EQ(cumulative_target(d("10"), weights, 1), d("1"));
    EXPECT_EQ(cumulative_target(d("10"), weights, 2), d("3"));
    EXPECT_EQ(cumulative_target(d("10"), weights, 3), d("6"));
    EXPECT_EQ(slice_size(d("10"), weights, 3), d("4"));
}

TEST(JoeVwap, UShapeIsHeavierAtBothEnds) {
    const auto weights = u_weights(6);
    EXPECT_EQ(weights.front(), weights.back());
    EXPECT_EQ(weights[1], weights[4]);
    EXPECT_GT(weights[0], weights[1]);
    EXPECT_GT(weights[1], weights[2]);
    EXPECT_EQ(weights[2], weights[3]);
}

TEST(JoeVwap, RecordedCurveWeightsSlicesByOverlap) {
    VolumeCurve curve;
    EXPECT_EQ(curve.weights(UnixNanos{0}, 3600 * k_s, 2), flat_weights(2));
    // 00:00-00:30 traded three times what 00:30-01:00 did
    curve.add(UnixNanos{60 * k_s}, d("3"));
    curve.add(UnixNanos{1900 * k_s}, d("1"));
    EXPECT_FALSE(curve.empty());
    const auto halves = curve.weights(UnixNanos{0}, 3600 * k_s, 2);
    EXPECT_EQ(halves[0], 3 * halves[1]);
    // a slice straddling the boundary takes half of each bucket
    const auto straddle = curve.weights(UnixNanos{900 * k_s}, 1800 * k_s, 1);
    EXPECT_EQ(straddle[0], static_cast<std::uint64_t>(d("2").raw));
    // the same half hour a day later lands in the same bucket
    const auto next_day = curve.weights(UnixNanos{86400 * k_s}, 3600 * k_s, 2);
    EXPECT_EQ(next_day, halves);
}

TEST(JoePov, TargetIsAShareOfMarketVolumeUpToTheTotal) {
    EXPECT_EQ(pov_target(d("5"), d("20"), 1000), d("2"));
    EXPECT_EQ(pov_target(d("5"), d("200"), 1000), d("5"));
}

ParentSpec spec() {
    ParentSpec out{};
    out.listing.symbol = *SymbolText::from("BTC/USDT");
    out.qty = d("0.1");
    out.horizon_ns = 60 * k_s;
    out.slices = 4;
    return out;
}

Child child(std::uint64_t id, std::int64_t lots) {
    return {.id = id, .instrument = venue(VenueId::Binance), .lot = d("0.001"), .lots = lots, .limit = d("100")};
}

TEST(JoeParent, PartialFillRollsTheRestBackIntoThePlan) {
    Parent parent{1, spec(), UnixNanos{1}};
    ASSERT_TRUE(parent.start(UnixNanos{2}, d("0.1"), d("100")).has_value());
    parent.add(child(11, 30));
    EXPECT_EQ(parent.working(), d("0.03"));
    EXPECT_EQ(parent.remaining(), d("0.07"));

    EXPECT_TRUE(parent.fill(11, d("0.01"), d("100"), d("0.0005")));
    EXPECT_EQ(parent.filled(), d("0.01"));
    EXPECT_EQ(parent.working(), d("0.02"));
    EXPECT_EQ(parent.remaining(), d("0.07"));

    // the ioc expires: its unfilled 0.02 is no longer working and is owed again
    EXPECT_TRUE(parent.close(11));
    EXPECT_EQ(parent.working(), d("0"));
    EXPECT_EQ(parent.remaining(), d("0.09"));
    EXPECT_EQ(next_slice(d("0.05"), parent.filled(), parent.working(), d("0.025"), 200, false), d("0.04"));

    EXPECT_FALSE(parent.fill(99, d("1"), d("1"), d("0")));
    EXPECT_EQ(parent.avg_price(), d("100"));
    EXPECT_EQ(parent.fees(), d("0.0005"));
}

TEST(JoeParent, TransitionsFollowTheMachine) {
    Parent parent{1, spec(), UnixNanos{1}};
    EXPECT_EQ(parent.pause(UnixNanos{2}).error(), TransitionError::NotWorking);
    EXPECT_EQ(parent.finish(UnixNanos{2}).error(), TransitionError::NotWorking);
    ASSERT_TRUE(parent.start(UnixNanos{2}, d("0.1"), d("100")).has_value());
    EXPECT_EQ(parent.start(UnixNanos{3}, d("0.1"), d("100")).error(), TransitionError::NotPending);
    EXPECT_EQ(parent.resume(UnixNanos{3}).error(), TransitionError::NotPaused);
    ASSERT_TRUE(parent.pause(UnixNanos{3}).has_value());
    EXPECT_EQ(parent.status(), Status::Paused);
    EXPECT_EQ(parent.finish(UnixNanos{3}).error(), TransitionError::NotWorking);
    ASSERT_TRUE(parent.resume(UnixNanos{4}).has_value());
    ASSERT_TRUE(parent.finish(UnixNanos{5}).has_value());
    EXPECT_EQ(parent.status(), Status::Done);
    EXPECT_EQ(parent.cancel(UnixNanos{6}).error(), TransitionError::Terminal);
    EXPECT_EQ(parent.ended().ns, 5U);
}

TEST(JoeParent, AFillAfterCancelStillCounts) {
    Parent parent{1, spec(), UnixNanos{1}};
    ASSERT_TRUE(parent.start(UnixNanos{2}, d("0.1"), d("100")).has_value());
    parent.add(child(11, 50));
    ASSERT_TRUE(parent.cancel(UnixNanos{3}).has_value());
    EXPECT_TRUE(parent.fill(11, d("0.05"), d("101"), d("0")));
    EXPECT_TRUE(parent.close(11));
    EXPECT_EQ(parent.status(), Status::Cancelled);
    EXPECT_EQ(parent.filled(), d("0.05"));
    EXPECT_EQ(parent.working(), d("0"));
}

TEST(JoeParent, FailKeepsItsReasonAndAnOverfillReadsAsNothingLeft) {
    Parent parent{1, spec(), UnixNanos{1}};
    ASSERT_TRUE(parent.start(UnixNanos{2}, d("0.01"), d("100")).has_value());
    parent.add(child(11, 10));
    parent.add(child(12, 10));
    EXPECT_TRUE(parent.fill(11, d("0.01"), d("100"), d("0")));
    EXPECT_TRUE(parent.fill(12, d("0.01"), d("100"), d("0")));
    EXPECT_EQ(parent.remaining(), d("0"));
    ASSERT_TRUE(parent.fail(UnixNanos{3}, "risk").has_value());
    EXPECT_EQ(parent.reason(), "risk");
    EXPECT_EQ(parent.fail(UnixNanos{4}, "again").error(), TransitionError::Terminal);
    EXPECT_EQ(parent.reason(), "risk");
}

TEST(JoeSpec, ValidateRefusesWhatCannotRun) {
    auto good = spec();
    EXPECT_TRUE(validate(good).has_value());
    auto both = good;
    both.notional = d("20");
    EXPECT_EQ(validate(both).error(), SpecError::BothSizes);
    auto none = good;
    none.qty = FixedE9::undef();
    EXPECT_EQ(validate(none).error(), SpecError::NoSize);
    auto no_horizon = good;
    no_horizon.horizon_ns = 0;
    EXPECT_EQ(validate(no_horizon).error(), SpecError::NoHorizon);
    no_horizon.algo = Algo::Sor;
    EXPECT_TRUE(validate(no_horizon).has_value());
    auto pov = good;
    pov.algo = Algo::Pov;
    EXPECT_EQ(validate(pov).error(), SpecError::NoRate);
}

} // namespace
} // namespace raikiri::joe
