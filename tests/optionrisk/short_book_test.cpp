#include <raikiri/optionrisk/short_book.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace raikiri::optionrisk {
namespace {

PositionRisk with(std::int64_t vega, std::int64_t gamma) {
    PositionRisk out{};
    out.vega_usd = FixedE9::from_raw(vega * k_e9);
    out.gamma_usd_1pct = FixedE9::from_raw(gamma * k_e9);
    return out;
}

TEST(ShortBook, GreeksNetAcrossTheBookAndOnlyShortsBreach) {
    const std::vector<PositionRisk> book{with(-300, -40), with(100, 10)};
    const auto greeks = book_greeks(book);
    EXPECT_EQ(greeks.vega_usd.raw, -200 * k_e9);
    EXPECT_EQ(greeks.gamma_usd_1pct.raw, -30 * k_e9);
    const auto at = [](std::int64_t v) { return FixedE9::from_raw(v * k_e9); };
    EXPECT_FALSE(past_short_limits(greeks, at(200), at(30)));
    EXPECT_TRUE(past_short_limits(greeks, at(199), at(30)));
    EXPECT_TRUE(past_short_limits(greeks, at(200), at(29)));
    EXPECT_FALSE(past_short_limits(greeks, FixedE9::undef(), FixedE9::undef()));
    const std::vector<PositionRisk> longs{with(500, 50)};
    EXPECT_FALSE(past_short_limits(book_greeks(longs), at(1), at(1)));
}

TEST(ShortBook, AnUnvaluedPositionFailsClosedOnlyWhenALimitIsSet) {
    const std::vector<PositionRisk> book{with(-1, -1), PositionRisk{}};
    const auto greeks = book_greeks(book);
    EXPECT_EQ(greeks.incomplete, 1U);
    EXPECT_TRUE(past_short_limits(greeks, FixedE9::from_raw(1000 * k_e9), FixedE9::undef()));
    EXPECT_FALSE(past_short_limits(greeks, FixedE9::undef(), FixedE9::undef()));
}

} // namespace
} // namespace raikiri::optionrisk
