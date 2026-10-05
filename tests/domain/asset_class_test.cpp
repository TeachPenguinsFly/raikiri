#include <raikiri/domain/asset_class.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string>

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
    const auto mexc = classified(VenueId::Mexc, "TESLA_USDT", "TESLA/USDT", AssetClass::Equity);
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
    const auto mexc_coin = classified(VenueId::Mexc, "SPX_USDT", "SPX/USDT", AssetClass::Crypto);
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

InstrumentBase stated(VenueId venue,
                      std::string_view exchange_symbol,
                      std::string_view local_symbol,
                      AssetClass cls,
                      std::string_view underlying) {
    InstrumentBase base{};
    base.exchange_symbol = *SymbolText::from(exchange_symbol);
    base.local_symbol = *SymbolText::from(local_symbol);
    base.asset_class = cls;
    base.underlying = *UnderlyingText::from(underlying);
    classify(venue, base);
    return base;
}

TEST(AssetKey, Hip3DeployersShareTheKeyAndKeepTheirPrefix) {
    const auto xyz = classified(VenueId::Hyperliquid, "xyz:TSLA", "xyz:TSLA/USDC", AssetClass::Equity);
    const auto para = classified(VenueId::Hyperliquid, "para:TSLA", "para:TSLA/USDC", AssetClass::Equity);
    const auto bybit = classified(VenueId::Bybit, "TSLAUSDT", "TSLA/USDT", AssetClass::Equity);
    EXPECT_EQ(asset_key(xyz), "equity:TSLA");
    EXPECT_EQ(market_key(xyz), market_key(para));
    EXPECT_EQ(market_key(xyz), market_key(bybit));
    EXPECT_EQ(dex_of(xyz), "xyz");
    EXPECT_EQ(dex_of(para), "para");
    EXPECT_EQ(dex_of(bybit), "");
    EXPECT_EQ(xyz.local_symbol.view(), "xyz:TSLA/USDC");

    const auto gold = classified(VenueId::Hyperliquid, "xyz:GOLD", "xyz:GOLD/USDC", AssetClass::Commodity);
    const auto silver = classified(VenueId::Hyperliquid, "xyz:SILVER", "xyz:SILVER/USDC", AssetClass::Commodity);
    EXPECT_EQ(asset_key(gold), "commodity:XAU");
    EXPECT_EQ(asset_key(silver), "commodity:XAG");
    // this deployer's index trades at the fund's price, not the index's
    const auto us500 = classified(VenueId::Hyperliquid, "mkts:US500", "mkts:US500/USDC", AssetClass::Index);
    EXPECT_EQ(asset_key(us500), "equity:SPY");
}

TEST(AssetKey, MetalNamesAliasOnlyWhereTheListingIsTheMetal) {
    const auto mexc = stated(VenueId::Mexc, "SILVER_USDT", "SILVER/USDT", AssetClass::Commodity, "XAG");
    const auto lbank = classified(VenueId::Lbank, "SILVERUSDT", "SILVER/USDT", AssetClass::Commodity);
    const auto weex = classified(VenueId::Weex, "XAGUSDT", "XAG/USDT", AssetClass::Commodity);
    EXPECT_EQ(asset_key(mexc), "commodity:XAG");
    EXPECT_EQ(asset_key(lbank), "commodity:XAG");
    EXPECT_EQ(asset_key(weex), "commodity:XAG");
    const auto coin = classified(VenueId::Binance, "GOLDUSDT", "GOLD/USDT", AssetClass::Crypto);
    EXPECT_EQ(asset_key(coin), "crypto:GOLD") << "a coin called GOLD is not gold";
}

TEST(AssetKey, GoldAndItsTwoTokensStayApart) {
    const auto xau = classified(VenueId::Binance, "XAUUSDT", "XAU/USDT", AssetClass::Commodity);
    // venues that file the tokens under metals
    const auto xaut = classified(VenueId::Weex, "XAUTUSDT", "XAUT/USDT", AssetClass::Commodity);
    const auto paxg = classified(VenueId::Kucoin, "PAXGUSDTM", "PAXG/USDT", AssetClass::Commodity);
    const auto mexc_xaut = stated(VenueId::Mexc, "XAUT_USDC", "XAUT/USDC", AssetClass::Commodity, "XAUT");
    const auto bingx = classified(VenueId::Bingx, "PAXG-USDT", "PAXG/USDT");
    EXPECT_EQ(asset_key(xau), "commodity:XAU");
    EXPECT_EQ(asset_key(xaut), "crypto:XAUT");
    EXPECT_EQ(asset_key(mexc_xaut), "crypto:XAUT");
    EXPECT_EQ(asset_key(paxg), "crypto:PAXG");
    EXPECT_EQ(asset_key(bingx), "crypto:PAXG");
    for (const auto& row : curated_rows()) {
        if (row.symbol.find("PAXG") != std::string_view::npos || row.symbol.find("XAUT") != std::string_view::npos) {
            EXPECT_NE(row.underlying, "XAU") << row.symbol;
        }
    }
}

TEST(AssetKey, AStockPairsAcrossMexcWeexAndHip3) {
    const auto mexc = stated(VenueId::Mexc, "GOLDCIRSTOCK_USDT", "GOLDCIRSTOCK/USDT", AssetClass::Equity, "GOLDCIR");
    const auto weex = classified(VenueId::Weex, "GOLDCIRUSDT", "GOLDCIR/USDT", AssetClass::Equity);
    const auto xyz = classified(VenueId::Hyperliquid, "xyz:GOLDCIR", "xyz:GOLDCIR/USDC", AssetClass::Equity);
    EXPECT_EQ(market_key(mexc), "equity:GOLDCIR/USD*");
    EXPECT_EQ(market_key(weex), market_key(mexc));
    EXPECT_EQ(market_key(xyz), market_key(mexc));
    EXPECT_EQ(mexc.local_symbol.view(), "GOLDCIRSTOCK/USDT") << "the venue-exact symbol is left alone";
    const auto stocker = stated(VenueId::Mexc, "STOCKER_USDT", "STOCKER/USDT", AssetClass::Crypto, "STOCKER");
    EXPECT_EQ(asset_key(stocker), "crypto:STOCKER");
}

TEST(AssetKey, ABareCurrencyIsItsDollarPrice) {
    const auto mexc = classified(VenueId::Mexc, "JPY_USDT", "JPY/USDT", AssetClass::FX);
    const auto xyz = classified(VenueId::Hyperliquid, "xyz:JPY", "xyz:JPY/USDC", AssetClass::FX);
    const auto lighter = classified(VenueId::Lighter, "USDJPY", "USDJPY/USDC");
    EXPECT_EQ(asset_key(mexc), "fx:JPYUSD");
    EXPECT_EQ(asset_key(xyz), "fx:USDJPY") << "this deployer quotes yen per dollar";
    EXPECT_EQ(asset_key(lighter), "fx:USDJPY");
}

TEST(AssetClass, AnUnderlyingTooLongToKeyIsNotPaired) {
    const auto long_name = classified(VenueId::Okx, "X-USDT-SWAP", "ABCDEFGHIJKLMNOPQ/USDT", AssetClass::Equity);
    EXPECT_EQ(long_name.asset_class, AssetClass::Unknown);
}

std::string undashed(std::string symbol) {
    std::erase(symbol, '-');
    return symbol;
}

std::string base_of_symbol(std::string_view symbol) {
    for (const std::string_view prefix : {"PF_", "PI_", "FF_"}) {
        if (symbol.starts_with(prefix)) {
            symbol.remove_prefix(prefix.size());
        }
    }
    std::string base{symbol.substr(0, symbol.find_first_of("-_/: "))};
    for (auto& c : base) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    for (const std::string_view quote : {"USDT", "USDC", "USD", "PERP"}) {
        if (base.size() > quote.size() && base.ends_with(quote)) {
            base.resize(base.size() - quote.size());
            break;
        }
    }
    return base;
}

TEST(AssetClass, NoFixtureSymbolNamedLikeAStatedUnderlyingDefaultsToCrypto) {
    std::set<std::string, std::less<>> known;
    for (const auto& row : curated_rows()) {
        if (row.cls != AssetClass::Crypto && row.cls != AssetClass::Unknown) {
            known.emplace(row.underlying);
        }
    }
    const std::regex field{R"re("(symbol|contractName|ticker|instrument_name|instrument|name)"\s*:\s*"([^"]+)")re"};
    const auto venues_dir = std::filesystem::path{__FILE__}.parent_path().parent_path() / "venues";
    std::size_t checked = 0;
    for (std::size_t v = 1; v < k_venue_count; ++v) {
        const auto venue = static_cast<VenueId>(v);
        if (venue == VenueId::Sim || is_retired(venue) || states_asset_class(venue)) {
            continue;
        }
        const auto dir = venues_dir / std::string{to_string(venue)} / "fixtures";
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator{dir, ec}) {
            const auto name = entry.path().filename().string();
            if (entry.path().extension() != ".json" || name.find("error") != std::string::npos) {
                continue;
            }
            std::ifstream in{entry.path()};
            const std::string text{std::istreambuf_iterator<char>{in}, {}};
            for (auto it = std::sregex_iterator{text.begin(), text.end(), field}; it != std::sregex_iterator{}; ++it) {
                const auto symbol = (*it)[2].str();
                if (symbol.find(':') != std::string::npos || !known.contains(base_of_symbol(symbol))) {
                    continue;
                }
                ++checked;
                const bool explicit_row = std::ranges::any_of(curated_rows(), [&](const CuratedRow& row) {
                    return row.venue == venue && (row.symbol == symbol || row.symbol == undashed(symbol));
                });
                EXPECT_TRUE(explicit_row)
                    << to_string(venue) << " " << symbol << " in " << name << " would default to crypto";
            }
        }
    }
    EXPECT_GT(checked, 0U);
}

} // namespace
