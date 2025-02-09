#include <raikiri/domain/identifiers.hpp>

#include <gtest/gtest.h>

namespace raikiri {
namespace {

TEST(InstrumentId, PackRoundTrip) {
    const auto id = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{12345});
    EXPECT_EQ(id.venue(), VenueId::Bybit);
    EXPECT_EQ(id.market(), MarketType::Perp);
    EXPECT_EQ(id.symbol().raw, 12345U);
    EXPECT_TRUE(id.valid());
    EXPECT_FALSE(InstrumentId{}.valid());
}

TEST(InstrumentId, DistinctMarketsDistinctIds) {
    const auto perp = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{1});
    const auto spot = InstrumentId::make(VenueId::Bybit, MarketType::Spot, SymbolId{1});
    EXPECT_NE(perp, spot);
}

TEST(VenueNames, RoundTrip) {
    for (std::uint8_t v = 1; v < k_venue_count; ++v) {
        const auto venue = static_cast<VenueId>(v);
        EXPECT_EQ(venue_from_string(to_string(venue)), venue);
    }
    EXPECT_EQ(venue_from_string("nope"), VenueId::Invalid);
    EXPECT_EQ(venue_from_string("invalid"), VenueId::Invalid);
}

TEST(AccountId, FormatsGoCompatible) {
    char buf[16];
    EXPECT_EQ(format_account({VenueId::Bybit, MarketType::Perp}, buf, sizeof buf), "bybit");
    EXPECT_EQ(format_account({VenueId::Bybit, MarketType::Spot}, buf, sizeof buf), "bybit-spot");
    EXPECT_EQ(format_account({VenueId::Binance, MarketType::Future}, buf, sizeof buf),
              "binance-fut");
}

TEST(ClientOrderId, PackRoundTrip) {
    const auto id = ClientOrderId::make(7, 123'456'789ULL);
    EXPECT_EQ(id.strategy_id(), 7);
    EXPECT_EQ(id.seq(), 123'456'789ULL);
    EXPECT_TRUE(id.valid());
    EXPECT_FALSE(ClientOrderId{}.valid());
}

TEST(ClientOrderId, SeqMasksTo48Bits) {
    const auto id = ClientOrderId::make(1, ~0ULL);
    EXPECT_EQ(id.seq(), 0xFFFF'FFFF'FFFFULL);
    EXPECT_EQ(id.strategy_id(), 1);
}

} // namespace
} // namespace raikiri
