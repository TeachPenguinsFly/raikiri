#include <raikiri/domain/asset_class.hpp>

#include <gtest/gtest.h>

namespace {

using namespace raikiri;

InstrumentBase classified(VenueId venue,
                          std::string_view exchange_symbol,
                          std::string_view local_symbol,
                          AssetClass stated = AssetClass::Unknown) {
    InstrumentBase base{};
    base.exchange_symbol = *SymbolText::from(exchange_symbol);
    base.local_symbol = *SymbolText::from(local_symbol);
    base.asset_class = stated;
    classify(venue, base);
    return base;
}

TEST(AssetClass, CuratedVenuesResolveTheSameUnderlyingUnderTheirOwnNames) {
    const auto lighter = classified(VenueId::Lighter, "TSLA", "TSLA/USDC");
    const auto aster = classified(VenueId::Aster, "TSLAUSDT", "TSLA/USDT");
    const auto mexc = classified(VenueId::Mexc, "TESLA_USDT", "TESLA/USDT");
    for (const auto& b : {lighter, aster, mexc}) {
        EXPECT_EQ(b.asset_class, AssetClass::Equity);
        EXPECT_EQ(b.underlying.view(), "TSLA");
    }
    const auto bitget = classified(VenueId::Bitget, "XAUUSDT", "XAU/USDT");
    EXPECT_EQ(bitget.asset_class, AssetClass::Commodity);
    EXPECT_EQ(bitget.underlying.view(), "XAU");
    const auto grvt = classified(VenueId::Grvt, "XAU_USDT_Perp", "XAU/USDT");
    EXPECT_EQ(grvt.asset_class, AssetClass::Commodity);
    EXPECT_EQ(grvt.underlying.view(), "XAU");
}

TEST(AssetClass, SpxTheCoinStaysCryptoWhereTheIndexIsCalledSomethingElse) {
    const auto coin = classified(VenueId::Lighter, "SPX", "SPX/USDC");
    EXPECT_EQ(coin.asset_class, AssetClass::Crypto);
    EXPECT_EQ(coin.underlying.view(), "SPX");
    const auto index = classified(VenueId::Lighter, "US500", "US500/USDC");
    EXPECT_EQ(index.asset_class, AssetClass::Index);
    EXPECT_EQ(index.underlying.view(), "SPX");
    const auto mexc_coin = classified(VenueId::Mexc, "SPX_USDT", "SPX/USDT");
    EXPECT_EQ(mexc_coin.asset_class, AssetClass::Crypto);
}

TEST(AssetClass, AStatingVenuesSilenceStaysUnknown) {
    const auto silent = classified(VenueId::Binance, "NEWUSDT", "NEW/USDT");
    EXPECT_EQ(silent.asset_class, AssetClass::Unknown);
    EXPECT_TRUE(silent.underlying.empty());
    const auto stated = classified(VenueId::Gate, "NAS100_USDT", "NAS100/USDT", AssetClass::Equity);
    EXPECT_EQ(stated.asset_class, AssetClass::Index);
    EXPECT_EQ(stated.underlying.view(), "NDX");
}

TEST(AssetClass, AnUnderlyingTooLongToKeyIsNotPaired) {
    const auto long_name =
        classified(VenueId::Okx, "X-USDT-SWAP", "ABCDEFGHIJKLMNOPQ/USDT", AssetClass::Equity);
    EXPECT_EQ(long_name.asset_class, AssetClass::Unknown);
}

} // namespace
