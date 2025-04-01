#include <raikiri/shm/catalogue.hpp>

#include <gtest/gtest.h>

#include <memory>

namespace raikiri::shm {
namespace {

Instrument perp(VenueId venue, std::uint32_t slot, std::string_view symbol) {
    PerpInstrument p{};
    p.base.id = InstrumentId::make(venue, MarketType::Perp, SymbolId{slot});
    p.base.exchange_symbol = *SymbolText::from(symbol);
    p.base.tick_size = FixedE9::from_raw(100'000);
    p.base.lot_size = FixedE9::from_raw(1'000'000);
    return p;
}

// 600 KiB of table is too much for a test stack frame.
std::unique_ptr<CatalogueTable> make_table() {
    return std::make_unique<CatalogueTable>();
}

TEST(Catalogue, PublishesInSlotOrder) {
    const auto table = make_table();
    ASSERT_EQ(table->size(), 0U);
    ASSERT_TRUE(table->push(perp(VenueId::Bybit, 0, "BTCUSDT")).has_value());
    ASSERT_TRUE(table->push(perp(VenueId::Bybit, 1, "ETHUSDT")).has_value());
    EXPECT_EQ(table->size(), 2U);
    EXPECT_EQ(table->all().size(), 2U);
    EXPECT_EQ(base_of(table->all()[1]).exchange_symbol.view(), "ETHUSDT");
}

TEST(Catalogue, RefusesAnInstrumentThatDisagreesWithItsSlot) {
    // Slot order IS the numbering. Accepting this would hand the consumer a
    // table where index and SymbolId disagree, which is exactly the silent
    // wrong-instrument bug the channel exists to prevent.
    const auto table = make_table();
    const auto pushed = table->push(perp(VenueId::Bybit, 7, "BTCUSDT"));
    ASSERT_FALSE(pushed.has_value());
    EXPECT_EQ(pushed.error(), CatalogueError::SlotMismatch);
    EXPECT_EQ(table->size(), 0U);
}

TEST(Catalogue, FindMatchesTheWholePackedId) {
    const auto table = make_table();
    ASSERT_TRUE(table->push(perp(VenueId::Bybit, 0, "BTCUSDT")).has_value());

    const auto id = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{0});
    ASSERT_NE(table->find(id), nullptr);
    EXPECT_EQ(base_of(*table->find(id)).exchange_symbol.view(), "BTCUSDT");

    // Same slot, different market and different venue: both are ids minted
    // against some other gateway's numbering, so neither resolves here.
    EXPECT_EQ(table->find(InstrumentId::make(VenueId::Bybit, MarketType::Spot, SymbolId{0})),
              nullptr);
    EXPECT_EQ(table->find(InstrumentId::make(VenueId::Okx, MarketType::Perp, SymbolId{0})),
              nullptr);
}

TEST(Catalogue, UnpublishedSlotDoesNotResolve) {
    const auto table = make_table();
    ASSERT_TRUE(table->push(perp(VenueId::Sim, 0, "SIM1")).has_value());
    EXPECT_EQ(table->find(InstrumentId::make(VenueId::Sim, MarketType::Perp, SymbolId{1})),
              nullptr);
}

TEST(Catalogue, FillsToCapacityThenRefuses) {
    const auto table = make_table();
    for (std::uint32_t i = 0; i < k_catalogue_cells; ++i) {
        ASSERT_TRUE(table->push(perp(VenueId::Sim, i, "SYM")).has_value()) << i;
    }
    const auto overflow = table->push(perp(VenueId::Sim, k_catalogue_cells, "SYM"));
    ASSERT_FALSE(overflow.has_value());
    EXPECT_EQ(overflow.error(), CatalogueError::Full);
    EXPECT_EQ(table->size(), k_catalogue_cells);
}

} // namespace
} // namespace raikiri::shm
