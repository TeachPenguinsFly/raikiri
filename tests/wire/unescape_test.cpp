#include <raikiri/wire/json.hpp>
#include <raikiri/wire/malformed.hpp>

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>

namespace raikiri::wire {
namespace {

std::expected<std::string, Error> decoded(std::string_view escaped) {
    std::string out(escaped.size(), '\0');
    const auto text = unescape(escaped, out);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return std::string{*text};
}

TEST(Unescape, SimpleEscapes) {
    EXPECT_EQ(decoded(R"(a\"b\\c\/d\be\ff\ng\rh\ti)"), std::string{"a\"b\\c/d\be\ff\ng\rh\ti"});
    EXPECT_EQ(decoded("plain"), std::string{"plain"});
    EXPECT_EQ(decoded(""), std::string{});
}

TEST(Unescape, UnicodeToUtf8) {
    EXPECT_EQ(decoded(R"(\u0041\u00e9\u20AC)"), std::string{"A\xC3\xA9\xE2\x82\xAC"});
    EXPECT_EQ(decoded(R"(\ud83d\ude00!)"), std::string{"\xF0\x9F\x98\x80!"});
    EXPECT_EQ(decoded(R"(\u0000)"), std::string(1, '\0'));
}

TEST(Unescape, MalformedEscapesAreErrors) {
    for (const std::string_view bad : {R"(\)",
                                       R"(ab\)",
                                       R"(\x)",
                                       R"(\u)",
                                       R"(\u12)",
                                       R"(\u12g4)",
                                       R"(\ud83d)",
                                       R"(\ud83dx)",
                                       R"(\ud83d\n)",
                                       R"(\ud83d\u0041)",
                                       R"(\ude00)",
                                       R"(\ud83d\u)",
                                       R"(\ud83d\ude0)"}) {
        const auto text = decoded(bad);
        ASSERT_FALSE(text.has_value()) << bad;
        EXPECT_EQ(text.error(), Error::NotJson) << bad;
    }
}

TEST(Unescape, ShortStorageIsAnError) {
    std::array<char, 3> small{};
    EXPECT_EQ(unescape("abcd", small).error(), Error::TooManyEntries);
    EXPECT_EQ(unescape(R"(ab\u20AC)", small).error(), Error::TooManyEntries);
    EXPECT_EQ(unescape(R"(abc\n)", small).error(), Error::TooManyEntries);
    EXPECT_EQ(unescape("", std::span<char>{}).value(), "");
}

TEST(Unescape, StrIntoDecodesAField) {
    const std::string frame = R"({"name":"caf\u00e9 \"bar\"","n":1})";
    auto root = session().open_object(frame);
    ASSERT_TRUE(root.has_value());
    ondemand::value value;
    ASSERT_EQ(find(*root, "name").get(value), simdjson::SUCCESS);
    std::array<char, 64> out{};
    EXPECT_EQ(str_into(value, out).value(), "caf\xC3\xA9 \"bar\"");
}

TEST(Unescape, NestedDocumentParsesAndViewsLiveInStorage) {
    const std::string frame = R"({"topic":"t","data":"{\"a\":1,\"s\":\"x\\\"y\",\"rows\":[\"\\u00e9\"]}"})";
    auto root = session().open_object(frame);
    ASSERT_TRUE(root.has_value());
    EXPECT_EQ(req_str(*root, "topic").value(), "t");
    ondemand::value data;
    ASSERT_EQ(find(*root, "data").get(data), simdjson::SUCCESS);
    std::string storage;
    auto nested = open_nested_object(data, storage);
    ASSERT_TRUE(nested.has_value());
    EXPECT_EQ(opt_u64(*nested, "a").value(), 1U);
    const auto s = req_str(*nested, "s").value();
    EXPECT_EQ(s, R"(x\"y)");
    EXPECT_GE(s.data(), storage.data());
    EXPECT_LE(s.data() + s.size(), storage.data() + storage.size());
    auto rows = req_array(*nested, "rows");
    ASSERT_TRUE(rows.has_value());
    std::array<char, 8> out{};
    std::size_t count = 0;
    for (auto row : *rows) {
        ondemand::value cell;
        ASSERT_EQ(row.get(cell), simdjson::SUCCESS);
        EXPECT_EQ(str_into(cell, out).value(), "\xC3\xA9");
        ++count;
    }
    EXPECT_EQ(count, 1U);
}

TEST(Unescape, NestedArrayAndTwiceNestedInOneStorage) {
    const std::string frame = R"({"data":"{\"inner\":\"[1,2]\"}"})";
    auto root = session().open_object(frame);
    ASSERT_TRUE(root.has_value());
    ondemand::value data;
    ASSERT_EQ(find(*root, "data").get(data), simdjson::SUCCESS);
    std::string storage;
    auto middle = open_nested_object(data, storage);
    ASSERT_TRUE(middle.has_value());
    ondemand::value inner;
    ASSERT_EQ(find(*middle, "inner").get(inner), simdjson::SUCCESS);
    auto array = open_nested_array(inner, storage);
    ASSERT_TRUE(array.has_value());
    std::uint64_t sum = 0;
    for (auto item : *array) {
        ondemand::value v;
        ASSERT_EQ(item.get(v), simdjson::SUCCESS);
        sum += u64_of(v).value();
    }
    EXPECT_EQ(sum, 3U);
}

TEST(Unescape, NestedRefusesNonStrings) {
    for (const std::string_view frame :
         {R"({"data":null})", R"({"data":""})", R"({"data":{"a":1}})", R"({"data":"{\"a\":"})", R"({"data":"\u12"})"}) {
        auto root = session().open_object(frame);
        ASSERT_TRUE(root.has_value()) << frame;
        ondemand::value data;
        ASSERT_EQ(find(*root, "data").get(data), simdjson::SUCCESS) << frame;
        std::string storage;
        EXPECT_FALSE(open_nested_object(data, storage).has_value()) << frame;
    }
}

void every_string(std::string_view frame) {
    std::array<char, 256> out{};
    (void)unescape(frame, out);
    auto root = session().open_object(frame);
    if (!root.has_value()) {
        return;
    }
    (void)for_each_field(*root, [&](std::string_view, ondemand::value& value) { (void)str_into(value, out); });
}

void nested(std::string_view frame) {
    auto root = session().open_object(frame);
    if (!root.has_value()) {
        return;
    }
    ondemand::value data;
    if (find(*root, "data").get(data) != simdjson::SUCCESS) {
        return;
    }
    std::string storage;
    auto inner = open_nested_object(data, storage);
    if (!inner.has_value()) {
        return;
    }
    std::array<char, 64> out{};
    (void)for_each_field(*inner, [&](std::string_view, ondemand::value& value) { (void)str_into(value, out); });
}

TEST(Malformed, Unescape) {
    malformed::hammer_frame("unescape",
                            R"({"a":"q\"\\\/\b\f\n\r\t","u":"\u0041\u00e9\u20ac","p":"\ud83d\ude00\ud800\udc00",)"
                            R"("x":"tail\\","data":"{\"k\":\"v\\u00e9\",\"n\":[1,\"\\ud83d\\ude00\"]}"})",
                            {every_string, nested});
}

} // namespace
} // namespace raikiri::wire
