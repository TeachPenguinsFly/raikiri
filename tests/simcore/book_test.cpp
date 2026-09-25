#include <raikiri/simcore/book.hpp>

#include <gtest/gtest.h>

#include <cstdint>

namespace raikiri::simcore {
namespace {

[[nodiscard]] constexpr PriceTicks px(std::int64_t v) noexcept {
    return PriceTicks::from_raw(v);
}

[[nodiscard]] constexpr QtyLots lots(std::int64_t v) noexcept {
    return QtyLots::from_raw(v);
}

void expect_uncrossed(const Book& book) {
    const auto snap = book.bbo();
    if (snap.has_value() && !snap->bid.is_undef() && !snap->ask.is_undef()) {
        EXPECT_LT(snap->bid.raw, snap->ask.raw);
    }
}

void rest(Book& book, std::uint64_t id, std::uint32_t owner, Side side, PriceTicks price, QtyLots qty) {
    const auto r = book.submit_limit(id, owner, side, price, qty, /*post_only=*/false);
    ASSERT_EQ(r.disposition, Disposition::Rested);
    ASSERT_TRUE(r.fills.empty());
}

TEST(Book, BboEmptyBook) {
    Book book;
    EXPECT_FALSE(book.bbo().has_value());
    EXPECT_FALSE(book.contains(1));
    EXPECT_EQ(book.find(1), nullptr);
}

TEST(Book, BboOneSidedBook) {
    Book book;
    rest(book, 1, 1, Side::Buy, px(100), lots(5));
    const auto snap = book.bbo();
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->bid, px(100));
    EXPECT_EQ(snap->bid_qty, lots(5));
    EXPECT_TRUE(snap->ask.is_undef());
    EXPECT_TRUE(snap->ask_qty.is_undef());
}

TEST(Book, BboAggregatesBestLevelQty) {
    Book book;
    rest(book, 1, 1, Side::Sell, px(101), lots(5));
    rest(book, 2, 1, Side::Sell, px(101), lots(7));
    rest(book, 3, 1, Side::Sell, px(102), lots(9));
    rest(book, 4, 1, Side::Buy, px(99), lots(2));
    const auto snap = book.bbo();
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->ask, px(101));
    EXPECT_EQ(snap->ask_qty, lots(12)); // 5 + 7, best level only
    EXPECT_EQ(snap->bid, px(99));
    EXPECT_EQ(snap->bid_qty, lots(2));
    expect_uncrossed(book);
}

TEST(Book, PriceTimePriorityFifoWithinLevel) {
    Book book;
    rest(book, 1, 1, Side::Buy, px(100), lots(5));
    rest(book, 2, 2, Side::Buy, px(100), lots(7));
    const auto r = book.submit_limit(3, 3, Side::Sell, px(100), lots(8), false);
    ASSERT_EQ(r.fills.size(), 2U);
    EXPECT_EQ(r.fills[0].maker_id, 1U);
    EXPECT_EQ(r.fills[0].maker_owner, 1U);
    EXPECT_EQ(r.fills[0].qty, lots(5));
    EXPECT_EQ(r.fills[0].maker_leaves, lots(0));
    EXPECT_EQ(r.fills[1].maker_id, 2U);
    EXPECT_EQ(r.fills[1].qty, lots(3));
    EXPECT_EQ(r.fills[1].maker_leaves, lots(4));
    EXPECT_EQ(r.disposition, Disposition::Filled);
    EXPECT_EQ(r.remainder, lots(0));
    const auto snap = book.bbo();
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->bid, px(100));
    EXPECT_EQ(snap->bid_qty, lots(4));
    EXPECT_FALSE(book.contains(1));
    EXPECT_TRUE(book.contains(2));
    expect_uncrossed(book);
}

TEST(Book, TakerFillsAtRestingPricesAcrossLevels) {
    Book book;
    rest(book, 1, 1, Side::Sell, px(101), lots(5));
    rest(book, 2, 1, Side::Sell, px(102), lots(5));
    const auto r = book.submit_limit(3, 2, Side::Buy, px(103), lots(8), false);
    ASSERT_EQ(r.fills.size(), 2U);
    EXPECT_EQ(r.fills[0].price, px(101));
    EXPECT_EQ(r.fills[0].qty, lots(5));
    EXPECT_EQ(r.fills[1].price, px(102));
    EXPECT_EQ(r.fills[1].qty, lots(3));
    EXPECT_EQ(r.disposition, Disposition::Filled);
    const auto snap = book.bbo();
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->ask, px(102));
    EXPECT_EQ(snap->ask_qty, lots(2));
    expect_uncrossed(book);
}

TEST(Book, PartialFillLeavesCorrectRemainders) {
    Book book;
    rest(book, 1, 1, Side::Sell, px(101), lots(4));
    const auto r = book.submit_limit(2, 2, Side::Buy, px(101), lots(10), false);
    ASSERT_EQ(r.fills.size(), 1U);
    EXPECT_EQ(r.fills[0].qty, lots(4));
    EXPECT_EQ(r.fills[0].maker_leaves, lots(0));
    EXPECT_EQ(r.disposition, Disposition::Rested);
    EXPECT_EQ(r.remainder, lots(6));
    EXPECT_FALSE(book.contains(1));
    const auto* taker = book.find(2);
    ASSERT_NE(taker, nullptr);
    EXPECT_EQ(taker->leaves, lots(6));
    const auto snap = book.bbo();
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->bid, px(101));
    EXPECT_EQ(snap->bid_qty, lots(6));
    EXPECT_TRUE(snap->ask.is_undef());
    expect_uncrossed(book);
}

TEST(Book, PostOnlyCrossRejectedBookUntouched) {
    Book book;
    rest(book, 1, 1, Side::Sell, px(101), lots(5));
    const auto before = book.bbo();
    const auto r = book.submit_limit(2, 2, Side::Buy, px(101), lots(5), /*post_only=*/true);
    EXPECT_EQ(r.disposition, Disposition::RejectedPostOnly);
    EXPECT_TRUE(r.fills.empty());
    EXPECT_EQ(r.remainder, lots(5));
    EXPECT_EQ(book.bbo(), before); // nothing moved
    EXPECT_FALSE(book.contains(2));
    expect_uncrossed(book);
}

TEST(Book, PostOnlyNonCrossingRests) {
    Book book;
    rest(book, 1, 1, Side::Sell, px(101), lots(5));
    const auto r = book.submit_limit(2, 2, Side::Buy, px(100), lots(5), /*post_only=*/true);
    EXPECT_EQ(r.disposition, Disposition::Rested);
    EXPECT_TRUE(book.contains(2));
    expect_uncrossed(book);
}

TEST(Book, IocRemainderExpiresNeverRests) {
    Book book;
    rest(book, 1, 1, Side::Sell, px(101), lots(4));
    const auto r = book.submit_limit(2, 2, Side::Buy, px(101), lots(10), false, TimeInForce::Ioc);
    ASSERT_EQ(r.fills.size(), 1U);
    EXPECT_EQ(r.fills[0].qty, lots(4));
    EXPECT_EQ(r.disposition, Disposition::Expired);
    EXPECT_EQ(r.remainder, lots(6));
    EXPECT_FALSE(book.contains(2));       // the remainder did not rest
    EXPECT_FALSE(book.bbo().has_value()); // both sides now empty
}

TEST(Book, FokFillsAllOrNothing) {
    Book book;
    rest(book, 1, 1, Side::Sell, px(101), lots(4));
    const auto fail = book.submit_limit(2, 2, Side::Buy, px(101), lots(5), false, TimeInForce::Fok);
    EXPECT_EQ(fail.disposition, Disposition::Expired);
    EXPECT_TRUE(fail.fills.empty());
    EXPECT_EQ(fail.remainder, lots(5));
    ASSERT_TRUE(book.bbo().has_value());
    EXPECT_EQ(book.bbo()->ask_qty, lots(4)); // untouched
    const auto ok = book.submit_limit(3, 2, Side::Buy, px(101), lots(4), false, TimeInForce::Fok);
    EXPECT_EQ(ok.disposition, Disposition::Filled);
    ASSERT_EQ(ok.fills.size(), 1U);
    EXPECT_EQ(ok.fills[0].qty, lots(4));
}

TEST(Book, MarketOnThinBookPartialThenExpired) {
    Book book;
    rest(book, 1, 1, Side::Buy, px(99), lots(4));
    rest(book, 2, 1, Side::Buy, px(98), lots(3));
    const auto r = book.submit_market(3, 2, Side::Sell, lots(10));
    ASSERT_EQ(r.fills.size(), 2U);
    EXPECT_EQ(r.fills[0].price, px(99));
    EXPECT_EQ(r.fills[0].qty, lots(4));
    EXPECT_EQ(r.fills[1].price, px(98));
    EXPECT_EQ(r.fills[1].qty, lots(3));
    EXPECT_EQ(r.disposition, Disposition::Expired);
    EXPECT_EQ(r.remainder, lots(3));
    EXPECT_FALSE(book.contains(3));       // market orders never rest
    EXPECT_FALSE(book.bbo().has_value()); // bid side wiped out
}

TEST(Book, MarketOnEmptyBookExpires) {
    Book book;
    const auto r = book.submit_market(1, 1, Side::Buy, lots(5));
    EXPECT_TRUE(r.fills.empty());
    EXPECT_EQ(r.disposition, Disposition::Expired);
    EXPECT_EQ(r.remainder, lots(5));
}

TEST(Book, CancelRemovesEmptyLevel) {
    Book book;
    rest(book, 1, 1, Side::Buy, px(100), lots(5));
    rest(book, 2, 1, Side::Buy, px(99), lots(3));
    EXPECT_TRUE(book.cancel(1));
    const auto snap = book.bbo();
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->bid, px(99)); // 100 level disappeared entirely
    EXPECT_TRUE(book.cancel(2));
    EXPECT_FALSE(book.bbo().has_value());
    EXPECT_FALSE(book.cancel(1)); // already gone
}

TEST(Book, CancelKeepsLevelWithRemainingOrders) {
    Book book;
    rest(book, 1, 1, Side::Sell, px(101), lots(5));
    rest(book, 2, 1, Side::Sell, px(101), lots(7));
    EXPECT_TRUE(book.cancel(1));
    const auto snap = book.bbo();
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->ask, px(101));
    EXPECT_EQ(snap->ask_qty, lots(7));
}

TEST(Book, CancelUnknownReturnsFalse) {
    Book book;
    EXPECT_FALSE(book.cancel(42));
}

TEST(Book, AmendPriceChangeLosesTimePriority) {
    Book book;
    rest(book, 1, 1, Side::Buy, px(100), lots(5));
    rest(book, 2, 2, Side::Buy, px(100), lots(5));
    const auto away = book.amend(1, px(99), lots(5));
    ASSERT_TRUE(away.has_value());
    EXPECT_EQ(away->disposition, Disposition::Rested);
    const auto back = book.amend(1, px(100), lots(5));
    ASSERT_TRUE(back.has_value());
    const auto* o1 = book.find(1);
    const auto* o2 = book.find(2);
    ASSERT_NE(o1, nullptr);
    ASSERT_NE(o2, nullptr);
    EXPECT_GT(o1->seq, o2->seq); // priority lost
    const auto taker = book.submit_limit(3, 3, Side::Sell, px(100), lots(5), false);
    ASSERT_EQ(taker.fills.size(), 1U);
    EXPECT_EQ(taker.fills[0].maker_id, 2U); // order 2 now fills first
    expect_uncrossed(book);
}

TEST(Book, AmendQtyDecreaseKeepsPriority) {
    Book book;
    rest(book, 1, 1, Side::Buy, px(100), lots(5));
    rest(book, 2, 2, Side::Buy, px(100), lots(5));
    const std::uint64_t seq_before = book.find(1)->seq;
    const auto r = book.amend(1, px(100), lots(3));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->disposition, Disposition::Rested);
    EXPECT_EQ(r->remainder, lots(3));
    // a size-down keeps queue priority, as on real venues
    EXPECT_EQ(book.find(1)->seq, seq_before);
    EXPECT_EQ(book.find(1)->leaves, lots(3));
    const auto taker = book.submit_limit(3, 3, Side::Sell, px(100), lots(3), false);
    ASSERT_EQ(taker.fills.size(), 1U);
    EXPECT_EQ(taker.fills[0].maker_id, 1U); // still first in line
}

TEST(Book, AmendQtyIncreaseLosesPriority) {
    Book book;
    rest(book, 1, 1, Side::Buy, px(100), lots(5));
    rest(book, 2, 2, Side::Buy, px(100), lots(5));
    const auto r = book.amend(1, px(100), lots(8)); // size-up = replace
    ASSERT_TRUE(r.has_value());
    EXPECT_GT(book.find(1)->seq, book.find(2)->seq);
    const auto taker = book.submit_limit(3, 3, Side::Sell, px(100), lots(5), false);
    ASSERT_EQ(taker.fills.size(), 1U);
    EXPECT_EQ(taker.fills[0].maker_id, 2U);
}

TEST(Book, AmendToCrossingPriceExecutes) {
    Book book;
    rest(book, 1, 1, Side::Sell, px(101), lots(3));
    rest(book, 2, 2, Side::Buy, px(99), lots(5));
    const auto r = book.amend(2, px(102), lots(5));
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->fills.size(), 1U);
    EXPECT_EQ(r->fills[0].price, px(101)); // resting price, as always
    EXPECT_EQ(r->fills[0].qty, lots(3));
    EXPECT_EQ(r->disposition, Disposition::Rested);
    EXPECT_EQ(r->remainder, lots(2));
    const auto snap = book.bbo();
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->bid, px(102));
    EXPECT_EQ(snap->bid_qty, lots(2));
    EXPECT_TRUE(snap->ask.is_undef());
    expect_uncrossed(book);
}

TEST(Book, AmendErrors) {
    Book book;
    EXPECT_FALSE(book.amend(42, px(100), lots(5)).has_value()); // unknown
    EXPECT_EQ(book.amend(42, px(100), lots(5)).error(), BookError::UnknownOrder);
    rest(book, 1, 1, Side::Buy, px(100), lots(5));
    EXPECT_EQ(book.amend(1, px(100), lots(0)).error(), BookError::BadQty);
    EXPECT_EQ(book.amend(1, px(0), lots(5)).error(), BookError::BadQty);
    EXPECT_EQ(book.find(1)->leaves, lots(5));
}

} // namespace
} // namespace raikiri::simcore
