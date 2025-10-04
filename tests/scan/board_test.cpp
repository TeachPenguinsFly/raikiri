#include <raikiri/scan/board.hpp>

#include <gtest/gtest.h>

namespace raikiri::scan {
namespace {

constexpr std::int64_t k_e9 = 1'000'000'000;

[[nodiscard]] constexpr FixedE9 e9(std::int64_t whole, std::int64_t billionths = 0) {
    return FixedE9::from_raw(whole * k_e9 + billionths);
}

[[nodiscard]] Leg leg(VenueId venue,
                      std::int64_t price,
                      std::int64_t rate_billionths,
                      std::uint32_t interval_s,
                      std::int64_t volume = 1'000'000) {
    return Leg{.venue = venue,
               .instrument = InstrumentId::make(venue, MarketType::Perp, SymbolId{1}),
               .bid = e9(price),
               .ask = e9(price),
               .rate = FixedE9::from_raw(rate_billionths),
               .interval_s = interval_s,
               .next_funding = UnixNanos{0},
               .volume_24h_quote = e9(volume)};
}

TEST(ScanBoard, AnHourlyVenueIsNotComparedAsIfItWereEightHourly) {
    // THE DEFECT THE REFERENCE SHIPS. Its headline number adds two raw
    // per-interval rates, so hyperliquid funding every hour and binance funding
    // every eight are treated as if their rates covered the same span. Here the
    // same stated rate on an hourly venue is worth eight times as much.
    const auto hourly = per_hour(FixedE9::from_raw(100'000), 3600);
    const auto eight = per_hour(FixedE9::from_raw(100'000), 8 * 3600);
    ASSERT_TRUE(hourly.has_value());
    ASSERT_TRUE(eight.has_value());
    EXPECT_EQ(hourly->raw, 100'000);
    EXPECT_EQ(eight->raw, 12'500) << "one eighth of the stated rate per hour";
    EXPECT_EQ(hourly->raw, eight->raw * 8);
}

TEST(ScanBoard, AVenueThatWillNotStateItsIntervalIsRefusedRatherThanAssumed) {
    // kraken states no interval at all. Defaulting it to eight hours would
    // understate an hourly venue eightfold and overstate a daily one — and the
    // board would carry the number as though it were known.
    EXPECT_FALSE(per_hour(FixedE9::from_raw(100'000), 0).has_value());

    const auto row = pair(leg(VenueId::Bybit, 100, 100'000, 8 * 3600),
                          leg(VenueId::Kraken, 100, 100'000, 0),
                          FixedE9::from_raw(0));
    ASSERT_FALSE(row.has_value());
    EXPECT_EQ(row.error(), Reject::NoInterval);
}

TEST(ScanBoard, TheRouteIsOrientedSoTheFundingEdgeIsPositive) {
    // A positive rate is longs paying shorts, so the leg to be short of is the
    // one that charges more. A board of negative edges is the same information
    // written backwards.
    const auto row = pair(leg(VenueId::Gate, 100, 300'000, 8 * 3600),
                          leg(VenueId::Bybit, 100, 100'000, 8 * 3600),
                          FixedE9::from_raw(0));
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->short_leg.venue, VenueId::Gate) << "charges the most; sell it";
    EXPECT_EQ(row->long_leg.venue, VenueId::Bybit) << "charges the least; hold it";
    EXPECT_GT(row->funding_edge_8h.raw, 0);
    EXPECT_EQ(row->funding_edge_8h.raw, 200'000) << "the difference over one settlement";
}

TEST(ScanBoard, OrientationFollowsTheNormalisedRateAndNotTheStatedOne) {
    // The two traps meeting: hyperliquid states the SMALLER number and is the
    // richer leg to be short of, because it collects it every hour.
    const auto row = pair(leg(VenueId::Hyperliquid, 100, 50'000, 3600),
                          leg(VenueId::Binance, 100, 200'000, 8 * 3600),
                          FixedE9::from_raw(0));
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->short_leg.venue, VenueId::Hyperliquid)
        << "0.005%/h beats 0.02%/8h, though the stated rate is a quarter of it";
    EXPECT_EQ(row->funding_edge_8h.raw, (50'000 - 25'000) * 8);
}

TEST(ScanBoard, ThePriceSpreadIsTakenAtTheSidesActuallyAvailable) {
    // Buy the long leg at its ASK, sell the short leg at its BID. Using mids
    // would price a route nobody can take and flatter every row by half the
    // two books' spreads.
    Leg cheap = leg(VenueId::Bybit, 100, 100'000, 8 * 3600);
    cheap.bid = e9(99);
    cheap.ask = e9(100); // we buy here
    Leg dear = leg(VenueId::Gate, 100, 300'000, 8 * 3600);
    dear.bid = e9(101); // we sell here
    dear.ask = e9(102);

    const auto row = pair(cheap, dear, FixedE9::from_raw(0));
    ASSERT_TRUE(row.has_value());
    ASSERT_EQ(row->short_leg.venue, VenueId::Gate);
    // (101 - 100) / ((101 + 100) / 2) = 0.995%, i.e. 99 bp.
    EXPECT_EQ(row->price_spread_bp, 99);
}

TEST(ScanBoard, ANegativePriceSpreadIsReportedRatherThanHidden) {
    // The route can cost something to get on to, and that is a real number the
    // funding has to beat. Clamping it at zero would make every row look free.
    Leg dear_to_buy = leg(VenueId::Bybit, 100, 100'000, 8 * 3600);
    dear_to_buy.bid = e9(100);
    dear_to_buy.ask = e9(101);
    Leg cheap_to_sell = leg(VenueId::Gate, 100, 300'000, 8 * 3600);
    cheap_to_sell.bid = e9(99);
    cheap_to_sell.ask = e9(100);

    const auto row = pair(dear_to_buy, cheap_to_sell, FixedE9::from_raw(0));
    ASSERT_TRUE(row.has_value());
    EXPECT_LT(row->price_spread_bp, 0) << "entering costs, and the row says so";
    EXPECT_GT(row->funding_edge_8h.raw, 0) << "while the funding still pays";
}

TEST(ScanBoard, TheTwoNumbersAreNeverAddedTogether) {
    // A pair whose funding pays while the perpetuals sit far apart is not an
    // arbitrage. Keeping the numbers separate is what lets a reader see that;
    // a single headline hides it, which is what the reference does.
    Leg a = leg(VenueId::Bybit, 100, 100'000, 8 * 3600);
    a.ask = e9(110); // a wide, expensive entry
    const auto row = pair(a, leg(VenueId::Gate, 100, 900'000, 8 * 3600), FixedE9::from_raw(0));
    ASSERT_TRUE(row.has_value());
    EXPECT_GT(row->funding_edge_8h.raw, 0);
    EXPECT_LT(row->price_spread_bp, 0);
    // Nothing in Row combines them: a reader has both or neither.
}

TEST(ScanBoard, AVenueAgainstItselfIsNotARoute) {
    const auto row = pair(leg(VenueId::Okx, 100, 300'000, 8 * 3600),
                          leg(VenueId::Okx, 100, 100'000, 8 * 3600),
                          FixedE9::from_raw(0));
    ASSERT_FALSE(row.has_value());
    EXPECT_EQ(row.error(), Reject::OneVenue);
}

TEST(ScanBoard, AMissingQuoteIsNotAZeroQuote) {
    // UNDEF IS NOT ZERO. A leg whose venue has said nothing has no price, and
    // pricing it at zero would put an infinite spread at the top of the board.
    Leg silent = leg(VenueId::Mexc, 100, 100'000, 8 * 3600);
    silent.bid = FixedE9::undef();
    const auto row =
        pair(silent, leg(VenueId::Gate, 100, 300'000, 8 * 3600), FixedE9::from_raw(0));
    ASSERT_FALSE(row.has_value());
    EXPECT_EQ(row.error(), Reject::NoQuote);
}

TEST(ScanBoard, TheVolumeFloorIsAppliedToBothLegsAndReportedAsTheThinner) {
    // A route is only as deep as the side that runs out first.
    const auto thin = pair(leg(VenueId::Bybit, 100, 100'000, 8 * 3600, 10),
                           leg(VenueId::Gate, 100, 300'000, 8 * 3600, 5'000'000),
                           e9(1'000'000));
    ASSERT_FALSE(thin.has_value());
    EXPECT_EQ(thin.error(), Reject::ThinVolume);

    const auto ok = pair(leg(VenueId::Bybit, 100, 100'000, 8 * 3600, 2'000'000),
                         leg(VenueId::Gate, 100, 300'000, 8 * 3600, 5'000'000),
                         e9(1'000'000));
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->volume_floor.raw, e9(2'000'000).raw) << "the thinner leg, not the sum";
}

TEST(ScanBoard, ACrossedBookIsRefusedRatherThanPriced) {
    Leg crossed = leg(VenueId::Xt, 100, 100'000, 8 * 3600);
    crossed.bid = e9(101);
    crossed.ask = e9(100);
    const auto row =
        pair(crossed, leg(VenueId::Gate, 100, 300'000, 8 * 3600), FixedE9::from_raw(0));
    ASSERT_FALSE(row.has_value());
    EXPECT_EQ(row.error(), Reject::CrossedBook);
}

TEST(ScanCarry, SpotIsNotAskedForAFundingIntervalItCannotHave) {
    // The refusal that would have killed every carry pair: spot pays no
    // funding, so it states no interval, and requiring one of both legs — as
    // the perpetual board does — would reject the whole board.
    Leg spot = leg(VenueId::Bybit, 100, 0, 0);
    spot.rate = FixedE9::undef();
    const auto row = carry(spot, leg(VenueId::Bybit, 100, 300'000, 8 * 3600), FixedE9::from_raw(0));
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->funding_8h.raw, 300'000) << "only the perpetual's rate enters";
}

TEST(ScanCarry, APerpetualWithoutAnIntervalIsStillRefused) {
    Leg spot = leg(VenueId::Bybit, 100, 0, 0);
    const auto row = carry(spot, leg(VenueId::Kraken, 100, 300'000, 0), FixedE9::from_raw(0));
    ASSERT_FALSE(row.has_value());
    EXPECT_EQ(row.error(), Reject::NoInterval);
}

TEST(ScanCarry, OneVenueIsAllowedBecauseThatIsTheClassicForm) {
    // The perpetual board refuses a venue against itself; this one must not.
    // Buying spot and shorting the perpetual on one exchange is the oldest
    // version of this trade, and often the only one whose legs share margin.
    Leg spot = leg(VenueId::Bybit, 100, 0, 0);
    const auto row = carry(spot, leg(VenueId::Bybit, 100, 300'000, 8 * 3600), FixedE9::from_raw(0));
    EXPECT_TRUE(row.has_value());
}

TEST(ScanCarry, TheBasisIsBuySpotAtTheAskAndSellThePerpAtTheBid) {
    Leg spot = leg(VenueId::Bybit, 100, 0, 0);
    spot.bid = e9(99);
    spot.ask = e9(100); // we buy here
    Leg perp = leg(VenueId::Bybit, 100, 300'000, 8 * 3600);
    perp.bid = e9(101); // we sell here
    perp.ask = e9(102);

    const auto row = carry(spot, perp, FixedE9::from_raw(0));
    ASSERT_TRUE(row.has_value());
    // (101 - 100) / ((101 + 100) / 2) = 0.995%, i.e. 99 bp.
    EXPECT_EQ(row->basis_bp, 99);
}

TEST(ScanCarry, ANegativeBasisIsReportedRatherThanHidden) {
    // The perpetual below spot means the carry costs something on the way in,
    // and the funding has to cover it before anything is earned.
    Leg spot = leg(VenueId::Bitget, 100, 0, 0);
    spot.bid = e9(100);
    spot.ask = e9(101);
    Leg perp = leg(VenueId::Bitget, 100, 300'000, 8 * 3600);
    perp.bid = e9(99);
    perp.ask = e9(100);

    const auto row = carry(spot, perp, FixedE9::from_raw(0));
    ASSERT_TRUE(row.has_value());
    EXPECT_LT(row->basis_bp, 0);
    EXPECT_GT(row->funding_8h.raw, 0);
}

TEST(ScanBoard, EveryRefusalNamesItself) {
    for (const auto why : {Reject::OneVenue,
                           Reject::NoInterval,
                           Reject::NoQuote,
                           Reject::ThinVolume,
                           Reject::CrossedBook}) {
        EXPECT_NE(to_string(why), "?");
    }
}

} // namespace
} // namespace raikiri::scan
