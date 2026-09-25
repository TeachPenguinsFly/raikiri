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
