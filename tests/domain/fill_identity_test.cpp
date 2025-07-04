// What keeps two fills apart in the stores that must not book one twice.
//
// The property under test is NOT uniqueness. A counter would be unique and
// would be wrong: a venue that resends a fill notification has to produce the
// identity it produced the first time, or the resend is booked as a second
// fill. Every test below is about determinism first and distinctness second.

#include <raikiri/domain/order_events.hpp>

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace {

using namespace raikiri;

constexpr auto k_order = ClientOrderId::make(7, 42);

TEST(FillIdentity, TheVenuesOwnIdIsUsedVerbatim) {
    // When the venue answers, nothing here has an opinion. Rewriting a venue's
    // id would break every join against what the venue itself reports.
    const auto id = fill_identity(k_order, QtyLots::from_raw(18), "83956769");
    EXPECT_EQ(id.view(), "83956769");
}

TEST(FillIdentity, WithoutOneTheAnswerSaysSo) {
    // '~' is not a character any venue mints, so a stored id always states
    // which half of the function produced it. Someone reading the executions
    // table can tell a venue's id from ours without consulting the adapter.
    const auto id = fill_identity(k_order, QtyLots::from_raw(18), "");
    ASSERT_FALSE(id.empty());
    EXPECT_EQ(id.view().front(), '~');
}

TEST(FillIdentity, AResendDerivesTheIdentityItDerivedBefore) {
    // The reason the fallback is derived and not minted. BingX's perpetual
    // push mints no trade id, so this is the only thing standing between a
    // resent fill and a second fee row.
    const auto first = fill_identity(k_order, QtyLots::from_raw(18), "");
    const auto again = fill_identity(k_order, QtyLots::from_raw(18), "");
    EXPECT_EQ(first.view(), again.view());
}

TEST(FillIdentity, EveryFillOfOneOrderGetsItsOwn) {
    // Leaves strictly decreases across an order's fills, which is what makes
    // it a fill discriminator rather than merely a quantity.
    std::set<std::string> seen;
    for (const std::int64_t leaves : {30, 18, 11, 4, 0}) {
        seen.insert(std::string{fill_identity(k_order, QtyLots::from_raw(leaves), "").view()});
    }
    EXPECT_EQ(seen.size(), 5U);
}

TEST(FillIdentity, TwoOrdersAtTheSameLeavesDoNotCollide) {
    const auto a = fill_identity(ClientOrderId::make(7, 42), QtyLots::from_raw(0), "");
    const auto b = fill_identity(ClientOrderId::make(7, 43), QtyLots::from_raw(0), "");
    EXPECT_NE(a.view(), b.view());
}

TEST(FillIdentity, FieldsAreFixedWidthSoTheyCannotBeReadTwoWays) {
    // A variable-length encoding would let (0x1a, 0xb) and (0x1, 0xab) render
    // the same. Both halves are always sixteen hex digits.
    const auto id = fill_identity(ClientOrderId{0x1A}, QtyLots::from_raw(0xB), "");
    EXPECT_EQ(id.view(), "~000000000000001a.000000000000000b");
}

TEST(FillIdentity, ANegativeLeavesStillRoundTripsDistinctly) {
    // leaves should never be negative, but QtyLots is signed and an adapter
    // that got it wrong must not silently alias onto a positive one.
    const auto negative = fill_identity(k_order, QtyLots::from_raw(-1), "");
    const auto positive = fill_identity(k_order, QtyLots::from_raw(1), "");
    EXPECT_NE(negative.view(), positive.view());
}

TEST(FillIdentity, AnOversizeVenueIdFallsBackRatherThanTruncating) {
    // Truncation would merge two fills into one row. The venue sending an id
    // longer than the field is a venue-shape change worth noticing, and the
    // derived identity is the answer that loses nothing.
    const std::string too_long(64, 'x');
    const auto id = fill_identity(k_order, QtyLots::from_raw(18), too_long);
    ASSERT_FALSE(id.empty());
    EXPECT_EQ(id.view().front(), '~');
}

} // namespace
