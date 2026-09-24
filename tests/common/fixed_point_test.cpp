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
    const std::string_view cases[] = {
        "0", "1", "-1", "0.0001", "-12.5", "3.141592653", "0.000000001"};
    char buf[32];
    for (const auto text : cases) {
        const auto parsed = parse_decimal_e9(text);
        ASSERT_TRUE(parsed.has_value()) << text;
        EXPECT_EQ(format_e9(*parsed, buf, sizeof buf), text);
    }
}

TEST(ParseNumberE9, ReadsTheSpellingsVenuesActuallySend) {
    EXPECT_EQ(parse_number_e9("1.0E-4")->raw, 100'000);           // bitmex, ws
    EXPECT_EQ(parse_number_e9("0.0001")->raw, 100'000);           // bitmex, rest
    EXPECT_EQ(parse_number_e9("6.4e4")->raw, 64'000'000'000'000); // deribit strike
    EXPECT_EQ(parse_number_e9("1.000000000E-9")->raw, 1);         // htx SHIB tick
    EXPECT_EQ(parse_number_e9("7e-05")->raw, 70'000);             // bingx min qty
}

TEST(ParseNumberE9, KrakensFundingNeedsTheRateReaderNotTheExactOne) {
    EXPECT_FALSE(parse_number_e9("-8.907125e-6").has_value());
    EXPECT_EQ(parse_rate_e9("-8.907125e-6")->raw, -8'907);
}

TEST(ParseNumberE9, AgreesWithThePlainReaderWhereBothApply) {
    for (const auto* text : {"0", "1", "64488.5", "0.001", "-12.25", "0.000000001"}) {
        const auto plain = parse_decimal_e9(text);
        const auto general = parse_number_e9(text);
        ASSERT_TRUE(plain.has_value()) << text;
        ASSERT_TRUE(general.has_value()) << text;
        EXPECT_EQ(plain->raw, general->raw) << text;
    }
}

TEST(ParseNumberE9, RefusesSignificanceBelowNineDigits) {
    EXPECT_FALSE(parse_number_e9("0.000000000001").has_value());
    EXPECT_FALSE(parse_number_e9("1.0E-10").has_value());
    EXPECT_EQ(parse_number_e9("0.0000000010000")->raw, 1);
}

TEST(ParseRateE9, TruncatesWhereExactWouldSilenceTheStream) {
    EXPECT_FALSE(parse_number_e9("-0.000058784590419109").has_value());
    EXPECT_EQ(parse_rate_e9("-0.000058784590419109")->raw, -58'784);
    // rounds toward zero
    EXPECT_EQ(parse_rate_e9("0.0000000019")->raw, 1);
    EXPECT_EQ(parse_rate_e9("-0.0000000019")->raw, -1);
}

TEST(ParseAmountE9, ReadsAWalletFigureTheExactReaderWouldRefuseEntirely) {
    EXPECT_FALSE(parse_number_e9("191.951084206297").has_value());
    EXPECT_EQ(parse_amount_e9("191.951084206297")->raw, 191'951'084'206);

    EXPECT_EQ(parse_amount_e9("0.0000000019")->raw, 1);
    EXPECT_EQ(parse_amount_e9("-0.0000000019")->raw, -1);

    EXPECT_EQ(parse_amount_e9("191.951084206")->raw, 191'951'084'206);

    for (const auto* text : {"", "abc", "1.2.3", "1,5"}) {
        EXPECT_FALSE(parse_amount_e9(text).has_value()) << text;
    }
}

TEST(ParseNumberE9, RejectsMalformedTokensRatherThanGuessing) {
    for (const auto* text : {"", ".", "-", "1.2.3", "1e", "1e+", "abc", "1 2", "0x10", "1,5"}) {
        EXPECT_FALSE(parse_number_e9(text).has_value()) << text;
        EXPECT_FALSE(parse_rate_e9(text).has_value()) << text;
    }
}

TEST(ParseNumberE9, RefusesWhatWouldOverflowTheRepresentation) {
    EXPECT_FALSE(parse_number_e9("1e30").has_value());
    EXPECT_FALSE(parse_number_e9("99999999999.999999999").has_value());
    EXPECT_FALSE(parse_number_e9("1e-40").has_value());
}

TEST(MulDiv, HoldsAProductThatDoesNotFitI64) {
    EXPECT_EQ(mul_div(500'000'000LL, 50'000'000'000'000LL, k_e9), 25'000'000'000'000LL);

    std::int64_t wrapped = 0;
    EXPECT_TRUE(__builtin_mul_overflow(500'000'000LL, 50'000'000'000'000LL, &wrapped));
}

TEST(MulDiv, AgreesWithA128BitReference) {
    constexpr std::int64_t cases[][3] = {
        {500'000'000, 50'000'000'000'000, k_e9},               // half a BTC at 50k
        {100'000'000'000, 100'000, k_e9},                      // gate: 100 contracts x 0.0001
        {100'000'000'000, 10'000'000'000, 50'000'000'000'000}, // deribit inverse
        {7, 3, 2},
        {-7, 3, 2},
        {7, -3, 2},
        {1, 1, k_e9}, // rounds to zero
    };
    for (const auto& c : cases) {
        const int128_t reference =
            (static_cast<int128_t>(c[0]) * static_cast<int128_t>(c[1])) / c[2];
        EXPECT_EQ(mul_div(c[0], c[1], c[2]), static_cast<std::int64_t>(reference))
            << c[0] << " * " << c[1] << " / " << c[2];
    }
}

TEST(MulDiv, TruncatesTowardZeroOnBothSigns) {
    EXPECT_EQ(mul_div(7, 1, 2), 3);
    EXPECT_EQ(mul_div(-7, 1, 2), -3);
}

} // namespace
} // namespace raikiri
