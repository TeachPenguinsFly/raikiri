#include <raikiri/common/fixed_point.hpp>

#include <gtest/gtest.h>

#include <string>

namespace raikiri {
namespace {

TEST(FixedPoint, DefaultIsUndef) {
    PriceTicks p{};
    EXPECT_TRUE(p.is_undef());
    EXPECT_FALSE(PriceTicks::from_raw(0).is_undef());
}

TEST(FixedPoint, ArithmeticAndOrdering) {
    const auto a = PriceTicks::from_raw(100);
    const auto b = PriceTicks::from_raw(42);
    EXPECT_EQ((a + b).raw, 142);
    EXPECT_EQ((a - b).raw, 58);
    EXPECT_EQ(a.scaled_by(3).raw, 300);
    EXPECT_LT(b, a);
}

TEST(FixedPoint, MulWideProducesExactProduct) {
    const auto n = mul_wide(9'000'000'000'000'000'000LL, 2);
    EXPECT_EQ(n.raw, static_cast<int128_t>(9'000'000'000'000'000'000LL) * 2);
    EXPECT_GT(n, Notional128{});
}

struct ParseCase {
    std::string_view text;
    std::int64_t want_raw;
};

TEST(ParseDecimalE9, HappyPaths) {
    const ParseCase cases[] = {
        {"0", 0},
        {"1", k_e9},
        {"-1", -k_e9},
        {"0.0001", 100'000},
        {"0.000000001", 1},
        {"-12.5", -12'500'000'000LL},
        {"3.141592653", 3'141'592'653LL},
        {"+2.5", 2'500'000'000LL},
        {"0.100000000000", 100'000'000}, // trailing zeros beyond 9 digits OK
    };
    for (const auto& c : cases) {
        const auto got = parse_decimal_e9(c.text);
        ASSERT_TRUE(got.has_value()) << c.text;
        EXPECT_EQ(got->raw, c.want_raw) << c.text;
    }
}

TEST(ParseDecimalE9, Rejections) {
    EXPECT_EQ(parse_decimal_e9("").error(), ParseError::Empty);
    EXPECT_EQ(parse_decimal_e9("-").error(), ParseError::Empty);
    EXPECT_EQ(parse_decimal_e9(".").error(), ParseError::Empty);
    EXPECT_EQ(parse_decimal_e9("1a").error(), ParseError::BadChar);
    EXPECT_EQ(parse_decimal_e9("1.2.3").error(), ParseError::BadChar);
    EXPECT_EQ(parse_decimal_e9("0.0000000001").error(), ParseError::TooManyFracDigits);
    EXPECT_EQ(parse_decimal_e9("99999999999999").error(), ParseError::OutOfRange);
}

TEST(FormatE9, RoundTripsParse) {
    const std::string_view cases[] = {"0", "1", "-1", "0.0001", "-12.5", "3.141592653",
                                      "0.000000001"};
    char buf[32];
    for (const auto text : cases) {
        const auto parsed = parse_decimal_e9(text);
        ASSERT_TRUE(parsed.has_value()) << text;
        EXPECT_EQ(format_e9(*parsed, buf, sizeof buf), text);
    }
}

} // namespace
} // namespace raikiri
