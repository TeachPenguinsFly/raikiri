#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace raikiri::book {
namespace {

constexpr auto k_instrument = InstrumentId::make(VenueId::Okx, MarketType::Perp, SymbolId{0});

Level lvl(std::int64_t price, std::int64_t qty) {
    return {PriceTicks::from_raw(price), QtyLots::from_raw(qty)};
}

TEST(Ladder, BidsDescendAndAsksAscendWhateverTheArrivalOrder) {
    Ladder bids{Side::Bid};
    for (const auto price : {100, 103, 101, 99, 102}) {
        bids.upsert(lvl(price, 1));
    }
    ASSERT_EQ(bids.size(), 5U);
    EXPECT_EQ(bids.levels()[0].price.raw, 103);
    EXPECT_EQ(bids.levels()[4].price.raw, 99);

    Ladder asks{Side::Ask};
    for (const auto price : {200, 197, 199, 201, 198}) {
        asks.upsert(lvl(price, 1));
    }
    EXPECT_EQ(asks.levels()[0].price.raw, 197);
    EXPECT_EQ(asks.levels()[4].price.raw, 201);
}

TEST(Ladder, UpsertReplacesRatherThanDuplicating) {
    Ladder bids{Side::Bid};
    bids.upsert(lvl(100, 5));
    bids.upsert(lvl(100, 7));
    ASSERT_EQ(bids.size(), 1U);
    EXPECT_EQ(bids.levels()[0].qty.raw, 7);
}

TEST(Ladder, RemovingAPriceThatIsNotThereIsNotAnError) {
    Ladder bids{Side::Bid};
    bids.upsert(lvl(100, 1));
    bids.remove(PriceTicks::from_raw(99));
    EXPECT_EQ(bids.size(), 1U);
    bids.remove(PriceTicks::from_raw(100));
    EXPECT_TRUE(bids.empty());
}

TEST(Ladder, ZeroQuantityIsStoredNotInterpreted) {
    // zero size, an explicit delete and a zero order count each mean delete on some venue
    Ladder bids{Side::Bid};
    bids.upsert(lvl(100, 0));
    ASSERT_EQ(bids.size(), 1U);
    EXPECT_EQ(bids.levels()[0].qty.raw, 0);
}

TEST(Ladder, TruncateKeepsTheTouchAndDropsTheFarEnd) {
    Ladder bids{Side::Bid};
    for (int i = 0; i < 10; ++i) {
        bids.upsert(lvl(100 - i, 1));
    }
    bids.truncate(3);
    ASSERT_EQ(bids.size(), 3U);
    EXPECT_EQ(bids.levels()[0].price.raw, 100) << "the touch survives truncation";
    EXPECT_EQ(bids.levels()[2].price.raw, 98);
}

TEST(Ladder, DropBeyondPrunesWhatNoSnapshotVouchedFor) {
    Ladder bids{Side::Bid};
    for (int i = 0; i < 5; ++i) {
        bids.upsert(lvl(100 - i, 1));
    }
    bids.drop_beyond(PriceTicks::from_raw(98));
    ASSERT_EQ(bids.size(), 3U);
    EXPECT_EQ(bids.levels()[2].price.raw, 98) << "the band edge is kept";

    Ladder asks{Side::Ask};
    for (int i = 0; i < 5; ++i) {
        asks.upsert(lvl(200 + i, 1));
    }
    asks.drop_beyond(PriceTicks::from_raw(202));
    ASSERT_EQ(asks.size(), 3U);
    EXPECT_EQ(asks.levels()[2].price.raw, 202);
}

TEST(Ladder, DropBeyondAgreesWithAScanFromTheTouchAtEveryBandEdge) {
    for (const auto side : {Side::Bid, Side::Ask}) {
        for (std::int64_t edge = 80; edge <= 220; ++edge) {
            Ladder ladder{side};
            for (std::int64_t price = 100; price <= 200; price += 3) {
                ladder.upsert(lvl(price, price));
            }
            std::vector<Level> expected;
            for (const auto& level : ladder.levels()) {
                const bool beyond = side == Side::Bid ? level.price.raw < edge : level.price.raw > edge;
                if (beyond) {
                    break;
                }
                expected.push_back(level);
            }
            ladder.drop_beyond(PriceTicks::from_raw(edge));
            ASSERT_EQ(ladder.size(), expected.size()) << "edge " << edge;
            for (std::size_t i = 0; i < expected.size(); ++i) {
                EXPECT_EQ(ladder.levels()[i].price.raw, expected[i].price.raw);
                EXPECT_EQ(ladder.levels()[i].qty.raw, expected[i].qty.raw);
            }
        }
        Ladder empty{side};
        empty.drop_beyond(PriceTicks::from_raw(100));
        EXPECT_TRUE(empty.empty());
    }
}

TEST(Ladders, AOneSidedBookPublishesNothing) {
    Ladders book;
    book.bids().upsert(lvl(100, 1));
    EXPECT_FALSE(book.snapshot(k_instrument, 1, UnixNanos{1}, UnixNanos{2}).has_value());

    book.asks().upsert(lvl(101, 1));
    EXPECT_TRUE(book.snapshot(k_instrument, 1, UnixNanos{1}, UnixNanos{2}).has_value());
}

TEST(Ladders, SnapshotCarriesAtMostTheDomainDepth) {
    Ladders book;
    for (std::size_t i = 0; i < k_book_levels + 20; ++i) {
        book.bids().upsert(lvl(static_cast<std::int64_t>(1000 - i), 1));
        book.asks().upsert(lvl(static_cast<std::int64_t>(2000 + i), 1));
    }
    const auto out = book.snapshot(k_instrument, 42, UnixNanos{7}, UnixNanos{9});
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->bid_count, k_book_levels);
    EXPECT_EQ(out->ask_count, k_book_levels);
    EXPECT_EQ(out->bids[0].price.raw, 1000) << "the touch, not an arbitrary level";
    EXPECT_EQ(out->asks[0].price.raw, 2000);
    EXPECT_LT(out->bids[0].price.raw, out->asks[0].price.raw);
    EXPECT_EQ(out->venue_seq, 42U);
    EXPECT_EQ(out->ts_event.ns, 7U);
}

TEST(ExactSteps, RefusesWhatDoesNotDivideRatherThanRounding) {
    const auto tick = FixedE9::from_raw(100'000'000); // 0.1
    EXPECT_EQ(exact_steps(*parse_decimal_e9("64488.5"), tick).value(), 644'885);
    EXPECT_FALSE(exact_steps(*parse_decimal_e9("64488.55"), tick).has_value())
        << "rounding here is how an order lands one tick from where it was meant";
    EXPECT_FALSE(exact_steps(FixedE9::from_raw(1), FixedE9::from_raw(0)).has_value());
}

TEST(ExactSteps, TakesTheValueFromWhicheverParserTheVenueNeeds) {
    const auto tick = FixedE9::from_raw(1); // 1e-9
    EXPECT_EQ(exact_steps(*parse_number_e9("1e-8"), tick).value(), 10);
}

} // namespace
} // namespace raikiri::book
