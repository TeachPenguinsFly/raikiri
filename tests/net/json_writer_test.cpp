#include <raikiri/net/json_writer.hpp>

#include <gtest/gtest.h>

#include <array>

namespace raikiri::net {
namespace {

TEST(JsonWriter, BuildsAnOrderPayload) {
    std::array<char, 256> buf{};
    JsonWriter w{buf};
    w.obj_begin()
        .str("category", "linear")
        .str("symbol", "BTCUSDT")
        .str("side", "Buy")
        .str("orderType", "Limit")
        .decimal("qty", *parse_decimal_e9("0.001"))
        .decimal("price", *parse_decimal_e9("50000.5"))
        .str("orderLinkId", "01ARZ3NDEKTSV4RRFFQ69G5FAV")
        .boolean("reduceOnly", false)
        .obj_end();

    ASSERT_TRUE(w.ok());
    EXPECT_EQ(w.view(),
              R"({"category":"linear","symbol":"BTCUSDT","side":"Buy","orderType":"Limit",)"
              R"("qty":"0.001","price":"50000.5","orderLinkId":"01ARZ3NDEKTSV4RRFFQ69G5FAV",)"
              R"("reduceOnly":false})");
}

TEST(JsonWriter, PricesRenderExactlyFromFixedPoint) {
    const std::string_view cases[] = {"0.000000001", "0.1", "12345678.123456789", "-0.5", "0"};
    for (const auto text : cases) {
        std::array<char, 128> buf{};
        JsonWriter w{buf};
        w.obj_begin().decimal("p", *parse_decimal_e9(text)).obj_end();
        ASSERT_TRUE(w.ok()) << text;
        EXPECT_EQ(w.view(), std::string{R"({"p":")"} + std::string{text} + R"("})") << text;
    }
}

TEST(JsonWriter, RawDecimalIsUnquoted) {
    std::array<char, 64> buf{};
    JsonWriter w{buf};
    w.obj_begin().decimal_raw("price", *parse_decimal_e9("7.25")).obj_end();
    ASSERT_TRUE(w.ok());
    EXPECT_EQ(w.view(), R"({"price":7.25})");
}

TEST(JsonWriter, NestedObjectsAndArrays) {
    std::array<char, 128> buf{};
    JsonWriter w{buf};
    w.obj_begin().num("id", 7).raw("params", R"({"a":1})").obj_end();
    ASSERT_TRUE(w.ok());
    EXPECT_EQ(w.view(), R"({"id":7,"params":{"a":1}})");
}

TEST(JsonWriter, EscapesHostileStrings) {
    std::array<char, 128> buf{};
    JsonWriter w{buf};
    w.obj_begin().str("s", "a\"b\\c\nd").obj_end();
    ASSERT_TRUE(w.ok());
    EXPECT_EQ(w.view(), R"({"s":"a\"b\\c\nd"})");
}

TEST(JsonWriter, OverflowIsStickyAndViewIsEmpty) {
    std::array<char, 16> buf{};
    JsonWriter w{buf};
    w.obj_begin().str("key", "a-value-that-does-not-fit-at-all").obj_end();
    EXPECT_FALSE(w.ok());
    EXPECT_TRUE(w.view().empty()) << "a truncated payload must never reach a venue";

    w.reset();
    w.obj_begin().num("i", 1).obj_end();
    EXPECT_TRUE(w.ok());
    EXPECT_EQ(w.view(), R"({"i":1})");
}

TEST(JsonWriter, UndefinedDecimalIsRefused) {
    std::array<char, 64> buf{};
    JsonWriter w{buf};
    w.obj_begin().decimal("price", FixedE9::undef()).obj_end();
    EXPECT_FALSE(w.ok());
}

TEST(JsonWriter, ExtremeValues) {
    std::array<char, 64> buf{};
    JsonWriter w{buf};
    w.obj_begin().num("min", INT64_MIN).num("max", INT64_MAX).obj_end();
    ASSERT_TRUE(w.ok());
    EXPECT_EQ(w.view(), R"({"min":-9223372036854775808,"max":9223372036854775807})");
}

} // namespace
} // namespace raikiri::net
