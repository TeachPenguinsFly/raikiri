#include <raikiri/admin/assets.hpp>

#include <gtest/gtest.h>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace raikiri::admin {
namespace {

std::string fixture(const std::string& name) {
    std::ifstream in{std::string{RAIKIRI_ADMIN_FIXTURES} + "/" + name, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

const std::string k_png = std::string{"\x89PNG\r\n\x1a\n"} + std::string(24, 'x');

struct Asked {
    std::string what;
    AssetFetched done;
};

// holds every question until the test answers it
class FakeUpstream final : public AssetUpstream {
public:
    void coin(std::string_view id, AssetFetched done) override {
        asked.push_back({"coin " + std::string{id}, std::move(done)});
    }

    void search(std::string_view symbol, AssetFetched done) override {
        asked.push_back({"search " + std::string{symbol}, std::move(done)});
    }

    void image(AssetImageKind kind, std::string_view path, AssetFetched done) override {
        asked.push_back({(kind == AssetImageKind::Stock ? "stock " : "image ") + std::string{path}, std::move(done)});
    }

    void answer(int status, std::string body) {
        auto next = std::move(asked.front());
        asked.erase(asked.begin());
        next.done(status, std::move(body));
    }

    std::vector<Asked> asked;
};

class AssetDeskTest : public ::testing::Test {
protected:
    AssetDeskTest() : dir_{std::filesystem::temp_directory_path() / ("raikiri-assets-" + std::to_string(::getpid()))} {
        std::filesystem::remove_all(dir_);
    }

    ~AssetDeskTest() override { std::filesystem::remove_all(dir_); }

    AssetProfileDone profile_into() {
        return [this](std::expected<AssetProfile, AssetError> result) { profiles_.push_back(std::move(result)); };
    }

    AssetIconDone icon_into() {
        return [this](std::expected<AssetIcon, AssetError> result) { icons_.push_back(std::move(result)); };
    }

    std::filesystem::path dir_;
    std::uint64_t now_{1'000'000'000ULL};
    FakeUpstream upstream_;
    AssetMarks marks_{*AssetMarks::parse(fixture("asset_marks.json"))};
    IconStore store_{dir_};
    AssetDesk desk_{upstream_,
                    &marks_,
                    &store_,
                    [this] { return MonoNanos{now_}; },
                    {.fresh_ns = 600, .profiles = 2, .missing_ns = 3'600, .failed_ns = 30}};
    std::vector<std::expected<AssetProfile, AssetError>> profiles_;
    std::vector<std::expected<AssetIcon, AssetError>> icons_;
};

TEST(AssetSymbol, TakesTickersAndRefusesPaths) {
    EXPECT_TRUE(valid_asset_symbol("BTC"));
    EXPECT_TRUE(valid_asset_symbol("1000PEPE"));
    EXPECT_TRUE(valid_asset_symbol("BRK.B"));
    EXPECT_TRUE(valid_asset_symbol("a_b-c"));
    EXPECT_FALSE(valid_asset_symbol(""));
    EXPECT_FALSE(valid_asset_symbol("."));
    EXPECT_FALSE(valid_asset_symbol(".."));
    EXPECT_FALSE(valid_asset_symbol(".hidden"));
    EXPECT_FALSE(valid_asset_symbol("a/b"));
    EXPECT_FALSE(valid_asset_symbol("a\\b"));
    EXPECT_FALSE(valid_asset_symbol("a b"));
    EXPECT_FALSE(valid_asset_symbol("a%2Fb"));
    EXPECT_FALSE(valid_asset_symbol(std::string_view{"a\0b", 3}));
    EXPECT_FALSE(valid_asset_symbol(std::string(k_asset_symbol_max + 1, 'A')));
}

TEST(AssetSymbol, MultipliersComeOff) {
    EXPECT_EQ(strip_multiplier("1000PEPE"), "PEPE");
    EXPECT_EQ(strip_multiplier("1000000MOG"), "MOG");
    EXPECT_EQ(strip_multiplier("10000SATS"), "SATS");
    EXPECT_EQ(strip_multiplier("1MBABYDOGE"), "BABYDOGE");
    EXPECT_EQ(strip_multiplier("kPEPE"), "PEPE");
    EXPECT_EQ(strip_multiplier("KAVA"), "KAVA");
    EXPECT_EQ(strip_multiplier("1000"), "1000");
    EXPECT_EQ(strip_multiplier("1INCH"), "1INCH");
}

TEST(AssetDecimal, PlainDigitsOnly) {
    EXPECT_EQ(asset_decimal(1733519864.0), "1733519864");
    EXPECT_EQ(asset_decimal(0.5), "0.5");
    EXPECT_EQ(asset_decimal(5.5142e-08), "0.000000055142");
    EXPECT_EQ(asset_decimal(4.2069e14), "420690000000000");
    EXPECT_EQ(asset_decimal(0.0), "");
    EXPECT_EQ(asset_decimal(-1.0), "");
    EXPECT_EQ(asset_decimal(std::numeric_limits<double>::infinity()), "");
    EXPECT_EQ(asset_decimal(std::numeric_limits<double>::quiet_NaN()), "");
}

TEST(AssetProfileParse, ShapesTheCatalogueDocument) {
    const auto profile = parse_asset_profile(fixture("asset_coin.json"), "PEPE");
    ASSERT_TRUE(profile.has_value());
    EXPECT_EQ(profile->symbol, "PEPE");
    EXPECT_EQ(profile->name, "Pepe");
    EXPECT_EQ(profile->rank, 31U);
    EXPECT_EQ(profile->market_cap, "1733519864");
    EXPECT_EQ(profile->fully_diluted, "1733519864.5");
    EXPECT_EQ(profile->circulating_supply, "420689899653542.56");
    EXPECT_EQ(profile->total_supply, "420690000000000");
    EXPECT_FALSE(profile->max_supply.has_value());
    EXPECT_EQ(profile->ath, "0.00002803");
    EXPECT_EQ(profile->ath_at, "2024-12-09T16:30:41.698Z");
    EXPECT_EQ(profile->atl, "0.000000055142");
    EXPECT_EQ(profile->atl_at, "2023-04-18T02:14:41.591Z");
    EXPECT_FALSE(profile->launched.has_value());
    // the first http address, not the first entry
    EXPECT_EQ(profile->links.website, "https://www.pepe.vip/");
    EXPECT_FALSE(profile->links.whitepaper.has_value());
    EXPECT_EQ(profile->links.explorer, "https://etherscan.io/token/0x6982508145454ce325ddbe47a25d4ec3d2311933");
    EXPECT_EQ(profile->categories, (std::vector<std::string>{"Meme", "Frog-Themed"}));
    EXPECT_EQ(profile->description, "Pepe is a meme coin on Ethereum.\r\nIt has 3 < 5 holders.");
}

TEST(AssetProfileParse, RefusesWhatIsNotACoin) {
    EXPECT_FALSE(parse_asset_profile(R"({"error":"coin not found"})", "X").has_value());
    EXPECT_FALSE(parse_asset_profile("<html>busy</html>", "X").has_value());
    EXPECT_FALSE(parse_asset_profile("", "X").has_value());
}

TEST(AssetProfileParse, BareDocumentKeepsTheSymbolAsName) {
    const auto profile = parse_asset_profile(R"({"id":"x","market_data":{"market_cap":{},"ath":{"usd":null}}})", "X");
    ASSERT_TRUE(profile.has_value());
    EXPECT_EQ(profile->name, "X");
    EXPECT_FALSE(profile->rank.has_value());
    EXPECT_FALSE(profile->market_cap.has_value());
    EXPECT_FALSE(profile->ath.has_value());
    EXPECT_TRUE(profile->description.empty());
}

TEST(AssetProfileParse, LongDescriptionIsCutOnACharacter) {
    std::string text;
    while (text.size() < k_asset_description_max + 100) {
        text += "\xd0\xb6"; // two bytes each
    }
    const auto profile = parse_asset_profile(R"({"id":"x","description":{"en":"a)" + text + R"("}})", "X");
    ASSERT_TRUE(profile.has_value());
    EXPECT_EQ(profile->description.size(), k_asset_description_max - 1);
    EXPECT_EQ(profile->description.back(), '\xb6');
}

TEST(AssetSearch, LargestCoinWithExactlyTheSymbol) {
    EXPECT_EQ(pick_asset_id(fixture("asset_search.json"), "PEPE"), "pepe");
    EXPECT_EQ(pick_asset_id(fixture("asset_search.json"), "pepecoin"), "pepecoin-2");
    EXPECT_EQ(pick_asset_id(fixture("asset_search.json"), "PEP"), "");
    EXPECT_EQ(pick_asset_id(R"({"coins":[{"id":"a","symbol":"X","market_cap_rank":null}]})", "X"), "a");
    EXPECT_FALSE(pick_asset_id(R"({"status":{"error_code":429}})", "X").has_value());
    EXPECT_FALSE(pick_asset_id("not json", "X").has_value());
}

TEST(AssetImage, KnownByItsLeadingBytes) {
    EXPECT_EQ(image_type(k_png), "image/png");
    EXPECT_EQ(image_type("\xff\xd8\xff\xe0 jfif"), "image/jpeg");
    EXPECT_EQ(image_type("GIF89a...."), "image/gif");
    EXPECT_EQ(image_type(std::string_view{"RIFF\x10\0\0\0WEBPVP8 ", 16}), "image/webp");
    EXPECT_EQ(image_type("<svg xmlns=\"http://www.w3.org/2000/svg\"/>"), "");
    EXPECT_EQ(image_type("<html>"), "");
    EXPECT_EQ(image_type(""), "");
}

TEST(AssetIconPath, StaysInsideTheDirectory) {
    const std::filesystem::path dir{"/var/cache/assets"};
    EXPECT_EQ(icon_path(dir, "PEPE"), dir / "PEPE");
    EXPECT_EQ(icon_path(dir, "BRK.B"), dir / "BRK.B");
    EXPECT_FALSE(icon_path(dir, "..").has_value());
    EXPECT_FALSE(icon_path(dir, "../passwd").has_value());
    EXPECT_FALSE(icon_path(dir, "/etc/passwd").has_value());
    EXPECT_FALSE(icon_path(dir, ".PEPE.part").has_value());
    EXPECT_FALSE(icon_path(dir, "").has_value());
}

TEST(AssetMarksFile, ReadsTheTerminalsMarks) {
    const auto marks = AssetMarks::parse(fixture("asset_marks.json"));
    ASSERT_TRUE(marks.has_value());
    EXPECT_EQ(marks->size(), 5U); // the ticker that is a path is dropped

    const auto pepe = marks->find("kPEPE");
    EXPECT_EQ(pepe.key, "PEPE");
    ASSERT_NE(pepe.mark, nullptr);
    EXPECT_EQ(pepe.mark->id, "pepe");
    EXPECT_EQ(pepe.mark->image, "29850/small/pepe-token.jpeg");
    EXPECT_FALSE(pepe.mark->stock);

    // a ticker listed with its multiplier is found as written
    EXPECT_EQ(marks->find("1000SATS").key, "1000SATS");
    EXPECT_EQ(marks->find("btc").key, "BTC");
    EXPECT_TRUE(marks->find("BTC").mark->image.empty());
    EXPECT_TRUE(marks->find("AAPL").mark->stock);
    EXPECT_TRUE(marks->find("BAD").mark->image.empty());

    const auto unknown = marks->find("1000NOPE");
    EXPECT_EQ(unknown.key, "NOPE");
    EXPECT_EQ(unknown.mark, nullptr);

    EXPECT_FALSE(AssetMarks::parse("[]").has_value());
}

TEST(AssetHostParse, HttpsOnly) {
    const auto host = AssetHost::parse("https://api.example.org/api/v3/");
    ASSERT_TRUE(host.has_value());
    EXPECT_EQ(host->host, "api.example.org");
    EXPECT_EQ(host->port, "443");
    EXPECT_EQ(host->prefix, "/api/v3");
    EXPECT_EQ(AssetHost::parse("https://images.example.org:8443")->port, "8443");
    EXPECT_TRUE(AssetHost::parse("https://images.example.org")->prefix.empty());
    EXPECT_FALSE(AssetHost::parse("http://api.example.org").has_value());
    EXPECT_FALSE(AssetHost::parse("api.example.org").has_value());
    EXPECT_FALSE(AssetHost::parse("https://").has_value());
    EXPECT_FALSE(AssetHost::parse("https://user@example.org").has_value());
    EXPECT_FALSE(AssetHost::parse("https://example.org:port").has_value());
}

TEST_F(AssetDeskTest, KnownCoinIsAskedOnceAndKept) {
    desk_.profile("kPEPE", profile_into());
    desk_.profile("PEPE", profile_into());
    ASSERT_EQ(upstream_.asked.size(), 1U);
    EXPECT_EQ(upstream_.asked.front().what, "coin pepe");
    EXPECT_TRUE(profiles_.empty());

    upstream_.answer(200, fixture("asset_coin.json"));
    ASSERT_EQ(profiles_.size(), 2U);
    ASSERT_TRUE(profiles_[0].has_value());
    EXPECT_EQ(profiles_[0]->name, "Pepe");
    EXPECT_TRUE(profiles_[1].has_value());

    now_ += 599;
    desk_.profile("PEPE", profile_into());
    EXPECT_TRUE(upstream_.asked.empty());
    EXPECT_EQ(profiles_.size(), 3U);
}

TEST_F(AssetDeskTest, StaleAnswerServedWhenTheCatalogueFails) {
    desk_.profile("PEPE", profile_into());
    upstream_.answer(200, fixture("asset_coin.json"));
    now_ += 600;

    desk_.profile("PEPE", profile_into());
    ASSERT_EQ(upstream_.asked.size(), 1U);
    upstream_.answer(429, "{}");
    ASSERT_EQ(profiles_.size(), 2U);
    EXPECT_TRUE(profiles_[1].has_value());

    // and the catalogue is left alone for a while
    now_ += 29;
    desk_.profile("PEPE", profile_into());
    EXPECT_TRUE(upstream_.asked.empty());
    now_ += 1;
    desk_.profile("PEPE", profile_into());
    EXPECT_EQ(upstream_.asked.size(), 1U);
}

TEST_F(AssetDeskTest, NoAnswerAndNothingKeptIsUnavailable) {
    desk_.profile("PEPE", profile_into());
    upstream_.answer(0, {});
    ASSERT_EQ(profiles_.size(), 1U);
    ASSERT_FALSE(profiles_[0].has_value());
    EXPECT_EQ(profiles_[0].error().status, 503);

    desk_.profile("PEPE", profile_into());
    upstream_.answer(200, "<html>busy</html>");
    EXPECT_EQ(profiles_[1].error().status, 503);
}

TEST_F(AssetDeskTest, UnlistedTickerIsSearchedFor) {
    desk_.profile("1000NOPE", profile_into());
    ASSERT_EQ(upstream_.asked.size(), 1U);
    EXPECT_EQ(upstream_.asked.front().what, "search NOPE");
    upstream_.answer(200, R"({"coins":[{"id":"nope-coin","symbol":"nope","market_cap_rank":900}]})");
    ASSERT_EQ(upstream_.asked.size(), 1U);
    EXPECT_EQ(upstream_.asked.front().what, "coin nope-coin");
    upstream_.answer(200, R"({"id":"nope-coin","name":"Nope"})");
    ASSERT_EQ(profiles_.size(), 1U);
    EXPECT_EQ(profiles_[0]->name, "Nope");
    EXPECT_EQ(profiles_[0]->symbol, "NOPE");
}

TEST_F(AssetDeskTest, UnknownAssetIsNotFoundAndRemembered) {
    desk_.profile("ZZZ", profile_into());
    upstream_.answer(200, R"({"coins":[]})");
    ASSERT_EQ(profiles_.size(), 1U);
    EXPECT_EQ(profiles_[0].error().status, 404);

    desk_.profile("ZZZ", profile_into());
    EXPECT_TRUE(upstream_.asked.empty());
    EXPECT_EQ(profiles_[1].error().status, 404);

    // a failed search says nothing about the coin
    desk_.profile("YYY", profile_into());
    upstream_.answer(429, "{}");
    EXPECT_EQ(profiles_[2].error().status, 503);
}

TEST_F(AssetDeskTest, StocksAndBadSymbolsNeverReachTheCatalogue) {
    desk_.profile("AAPL", profile_into());
    desk_.profile("../etc", profile_into());
    desk_.icon("../etc", icon_into());
    EXPECT_TRUE(upstream_.asked.empty());
    EXPECT_EQ(profiles_[0].error().status, 404);
    EXPECT_EQ(profiles_[1].error().status, 400);
    EXPECT_EQ(icons_[0].error().status, 400);
}

TEST_F(AssetDeskTest, KeepsNoMoreThanItsBound) {
    for (const std::string symbol : {"AAA", "BBB", "CCC"}) {
        desk_.profile(symbol, profile_into());
        upstream_.answer(200, R"({"coins":[]})");
        ++now_;
    }
    EXPECT_EQ(desk_.cached(), 2U);
    // the oldest went
    desk_.profile("AAA", profile_into());
    EXPECT_EQ(upstream_.asked.size(), 1U);
}

TEST_F(AssetDeskTest, ImageFetchedOnceThenServedFromDisk) {
    desk_.icon("PEPE", icon_into());
    desk_.icon("kPEPE", icon_into());
    ASSERT_EQ(upstream_.asked.size(), 1U);
    EXPECT_EQ(upstream_.asked.front().what, "image 29850/small/pepe-token.jpeg");
    upstream_.answer(200, k_png);
    ASSERT_EQ(icons_.size(), 2U);
    ASSERT_TRUE(icons_[0].has_value());
    EXPECT_EQ(icons_[0]->content_type, "image/png");
    EXPECT_EQ(icons_[0]->bytes, k_png);
    EXPECT_TRUE(std::filesystem::exists(dir_ / "PEPE"));

    // a desk started later finds it on disk
    AssetDesk later{upstream_, &marks_, &store_, [this] { return MonoNanos{now_}; }};
    later.icon("PEPE", icon_into());
    EXPECT_TRUE(upstream_.asked.empty());
    ASSERT_EQ(icons_.size(), 3U);
    EXPECT_EQ(icons_[2]->bytes, k_png);
}

TEST_F(AssetDeskTest, StockImageComesFromTheStockHost) {
    desk_.icon("AAPL", icon_into());
    ASSERT_EQ(upstream_.asked.size(), 1U);
    EXPECT_EQ(upstream_.asked.front().what, "stock AAPL.png");
}

TEST_F(AssetDeskTest, WhatIsNotAnImageIsNotKept) {
    desk_.icon("PEPE", icon_into());
    upstream_.answer(200, "<html>sign in</html>");
    ASSERT_EQ(icons_.size(), 1U);
    EXPECT_EQ(icons_[0].error().status, 404);
    EXPECT_FALSE(std::filesystem::exists(dir_ / "PEPE"));

    // not asked for again within the hour
    desk_.icon("PEPE", icon_into());
    EXPECT_TRUE(upstream_.asked.empty());
    now_ += 3'600;
    desk_.icon("PEPE", icon_into());
    ASSERT_EQ(upstream_.asked.size(), 1U);

    upstream_.answer(200, k_png + std::string(k_asset_icon_max, 'x'));
    EXPECT_EQ(icons_.back().error().status, 404);
    EXPECT_FALSE(std::filesystem::exists(dir_ / "PEPE"));
}

TEST_F(AssetDeskTest, SilentImageHostIsRetriedSoon) {
    desk_.icon("PEPE", icon_into());
    upstream_.answer(0, {});
    EXPECT_EQ(icons_[0].error().status, 503);
    now_ += 30;
    desk_.icon("PEPE", icon_into());
    EXPECT_EQ(upstream_.asked.size(), 1U);
}

TEST_F(AssetDeskTest, NoImageWithoutAStoreOrAMark) {
    AssetDesk off{upstream_, &marks_, nullptr, [this] { return MonoNanos{now_}; }};
    off.icon("PEPE", icon_into());
    desk_.icon("BTC", icon_into()); // bundled with the terminal
    desk_.icon("BAD", icon_into()); // its path climbs out of the image host's directory
    desk_.icon("NOPE", icon_into());
    EXPECT_TRUE(upstream_.asked.empty());
    ASSERT_EQ(icons_.size(), 4U);
    for (const auto& icon : icons_) {
        EXPECT_EQ(icon.error().status, 404);
    }
}

TEST_F(AssetDeskTest, RoutesAnswerProfilesAndImages) {
    const AssetRoutes routes{desk_};
    std::vector<http::Response> sent;
    const AssetRespond respond = [&sent](http::Response response) { sent.push_back(std::move(response)); };

    EXPECT_FALSE(routes({.method = "GET", .path = "/v1/accounts"}, respond));
    EXPECT_FALSE(routes({.method = "GET", .path = "/v1/assets/"}, respond));
    EXPECT_FALSE(routes({.method = "GET", .path = "/v1/assets/a/b/icon"}, respond));

    EXPECT_TRUE(routes({.method = "POST", .path = "/v1/assets/PEPE"}, respond));
    EXPECT_EQ(sent.back().status, 405);
    EXPECT_TRUE(routes({.method = "GET", .path = "/v1/assets/..%2Fx"}, respond));
    EXPECT_EQ(sent.back().status, 400);

    EXPECT_TRUE(routes({.method = "GET", .path = "/v1/assets/PEPE"}, respond));
    EXPECT_EQ(sent.size(), 2U);
    upstream_.answer(200, fixture("asset_coin.json"));
    ASSERT_EQ(sent.size(), 3U);
    EXPECT_EQ(sent.back().status, 200);
    EXPECT_EQ(sent.back().content_type, "application/json");
    EXPECT_NE(sent.back().body.find(R"("market_cap":"1733519864")"), std::string::npos);
    EXPECT_NE(sent.back().body.find(R"("links":{"website":"https://www.pepe.vip/")"), std::string::npos);
    EXPECT_EQ(sent.back().body.find("max_supply"), std::string::npos);

    EXPECT_TRUE(routes({.method = "GET", .path = "/v1/assets/PEPE/icon"}, respond));
    upstream_.answer(200, k_png);
    ASSERT_EQ(sent.size(), 4U);
    EXPECT_EQ(sent.back().status, 200);
    EXPECT_EQ(sent.back().content_type, "image/png");
    EXPECT_EQ(sent.back().body, k_png);
    EXPECT_FALSE(sent.back().cache_control.empty());
}

} // namespace
} // namespace raikiri::admin
