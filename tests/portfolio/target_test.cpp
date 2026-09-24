#include <raikiri/portfolio/target.hpp>

#include <gtest/gtest.h>

namespace raikiri::portfolio {
namespace {

InstrumentBase make(ContractKind kind,
                    std::string_view tick,
                    std::string_view lot,
                    std::string_view contract_value) {
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Binance, MarketType::Perp, SymbolId{1});
    base.local_symbol = *SymbolText::from("X");
    base.exchange_symbol = *SymbolText::from("X");
    base.tick_size = *parse_decimal_e9(tick);
    base.lot_size = *parse_decimal_e9(lot);
    base.contract_value = *parse_decimal_e9(contract_value);
    base.kind = kind;
    return base;
}

InstrumentBase binance_btcusdt() {
    return make(ContractKind::Linear, "0.1", "0.001", "1");
}

InstrumentBase bitmex_xbtusd() {
    return make(ContractKind::Inverse, "0.5", "1", "1");
}

QtyLots lots(std::int64_t n) {
    return QtyLots::from_raw(n);
}

TEST(Target, FlatBookAsksForTheWholeTarget) {
    const auto out = delta_to_target(lots(100), lots(0), lots(0), lots(0));
    EXPECT_EQ(out.side, Side::Buy);
    EXPECT_EQ(out.qty.raw, 100);
}

TEST(Target, ReachedTargetAsksForNothing) {
    const auto out = delta_to_target(lots(100), lots(100), lots(0), lots(0));
    EXPECT_EQ(out.qty.raw, 0);
}

TEST(Target, WorkingOrdersCountAsProgress) {
    const auto out = delta_to_target(lots(100), lots(40), lots(60), lots(0));
    EXPECT_EQ(out.qty.raw, 0);

    const auto partial = delta_to_target(lots(100), lots(40), lots(30), lots(0));
    EXPECT_EQ(partial.side, Side::Buy);
    EXPECT_EQ(partial.qty.raw, 30);
}

TEST(Target, ADroppedFillRepairsItselfOnTheNextEvaluation) {
    const auto out = delta_to_target(lots(100), lots(70), lots(0), lots(0));
    EXPECT_EQ(out.side, Side::Buy);
    EXPECT_EQ(out.qty.raw, 30);
}

TEST(Target, OvershootTurnsAround) {
    const auto out = delta_to_target(lots(100), lots(130), lots(0), lots(0));
    EXPECT_EQ(out.side, Side::Sell);
    EXPECT_EQ(out.qty.raw, 30);
}

TEST(Target, FlatteningAndFlippingAreOrdinaryTargets) {
    const auto flatten = delta_to_target(lots(0), lots(50), lots(0), lots(0));
    EXPECT_EQ(flatten.side, Side::Sell);
    EXPECT_EQ(flatten.qty.raw, 50);

    const auto flip = delta_to_target(lots(-50), lots(50), lots(0), lots(0));
    EXPECT_EQ(flip.side, Side::Sell);
    EXPECT_EQ(flip.qty.raw, 100);
}

TEST(Target, WorkingSellsCountAgainstAShortTarget) {
    const auto out = delta_to_target(lots(-100), lots(0), lots(0), lots(100));
    EXPECT_EQ(out.qty.raw, 0);
}

TEST(RiskSizing, OnePercentOfEquityOverTheStopDistance) {
    // 10 000 USDT risking 1% over a 1000 USDT stop; one lot loses 1 USDT, so 100 lots
    const auto base = binance_btcusdt();
    const auto at = [&](std::string_view p) { return *base.price_to_ticks(*parse_decimal_e9(p)); };

    const auto size = size_for_risk(
        base, *parse_decimal_e9("10000"), *parse_decimal_e9("0.01"), at("50000"), at("49000"));
    ASSERT_TRUE(size.has_value());
    EXPECT_EQ(size->raw, 100);
}

TEST(RiskSizing, ATighterStopBuysMoreSize) {
    const auto base = binance_btcusdt();
    const auto at = [&](std::string_view p) { return *base.price_to_ticks(*parse_decimal_e9(p)); };
    const auto wide = size_for_risk(
        base, *parse_decimal_e9("10000"), *parse_decimal_e9("0.01"), at("50000"), at("49000"));
    const auto tight = size_for_risk(
        base, *parse_decimal_e9("10000"), *parse_decimal_e9("0.01"), at("50000"), at("49500"));
    ASSERT_TRUE(wide.has_value() && tight.has_value());
    EXPECT_EQ(tight->raw, wide->raw * 2);
}

TEST(RiskSizing, DirectionOfTheStopDoesNotChangeTheSize) {
    const auto base = binance_btcusdt();
    const auto at = [&](std::string_view p) { return *base.price_to_ticks(*parse_decimal_e9(p)); };
    const auto below = size_for_risk(
        base, *parse_decimal_e9("10000"), *parse_decimal_e9("0.01"), at("50000"), at("49000"));
    const auto above = size_for_risk(
        base, *parse_decimal_e9("10000"), *parse_decimal_e9("0.01"), at("50000"), at("51000"));
    ASSERT_TRUE(below.has_value() && above.has_value());
    EXPECT_EQ(below->raw, above->raw);
}

TEST(RiskSizing, InverseIsSizedInItsOwnCurrency) {
    const auto base = bitmex_xbtusd();
    const auto at = [&](std::string_view p) { return *base.price_to_ticks(*parse_decimal_e9(p)); };
    const auto size = size_for_risk(
        base, *parse_decimal_e9("1"), *parse_decimal_e9("0.01"), at("50000"), at("60000"));
    ASSERT_TRUE(size.has_value());
    // 1/60000 lands on 16666666666 in e9, so the size truncates to 2999
    EXPECT_EQ(size->raw, 2999);
}

TEST(RiskSizing, RefusesInputsThatMakeTheAnswerMeaningless) {
    const auto base = binance_btcusdt();
    const auto at = [&](std::string_view p) { return *base.price_to_ticks(*parse_decimal_e9(p)); };
    const auto ok = *parse_decimal_e9("10000");
    const auto pct = *parse_decimal_e9("0.01");

    EXPECT_FALSE(size_for_risk(base, ok, pct, at("50000"), at("50000")).has_value());
    EXPECT_FALSE(
        size_for_risk(base, FixedE9::from_raw(0), pct, at("50000"), at("49000")).has_value());
    EXPECT_FALSE(
        size_for_risk(base, ok, FixedE9::from_raw(0), at("50000"), at("49000")).has_value());
    EXPECT_FALSE(size_for_risk(base, ok, pct, at("50000"), PriceTicks::undef()).has_value());
}

TEST(RiskSizing, TruncatesRatherThanRoundingUp) {
    const auto base = binance_btcusdt();
    const auto at = [&](std::string_view p) { return *base.price_to_ticks(*parse_decimal_e9(p)); };
    const auto size = size_for_risk(
        base, *parse_decimal_e9("10009"), *parse_decimal_e9("0.01"), at("50000"), at("49000"));
    ASSERT_TRUE(size.has_value());
    EXPECT_EQ(size->raw, 100); // 100.09 lots of budget, not 101
}

} // namespace
} // namespace raikiri::portfolio
