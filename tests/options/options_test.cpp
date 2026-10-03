#include <raikiri/common/fixed_point.hpp>
#include <raikiri/options/chain.hpp>
#include <raikiri/options/convention.hpp>
#include <raikiri/options/key.hpp>

#include <gtest/gtest.h>

#include <vector>

using namespace raikiri;
using namespace raikiri::options;

namespace {

// 2026-08-11 08:00:00 utc
constexpr UnixNanos k_aug11{1'786'435'200'000'000'000ULL};
constexpr UnixNanos k_aug12{k_aug11.ns + (86'400ULL * 1'000'000'000ULL)};

FixedE9 e9(const char* text) {
    return *parse_decimal_e9(text);
}

InstrumentBase base_for(VenueId venue, MarketType market, std::uint32_t symbol, const char* underlying) {
    InstrumentBase base{};
    base.id = InstrumentId::make(venue, market, SymbolId{symbol});
    base.underlying = *UnderlyingText::from(underlying);
    base.tick_size = e9("0.0001");
    base.lot_size = e9("0.1");
    return base;
}

Instrument option(
    VenueId venue, std::uint32_t symbol, const char* underlying, UnixNanos expiry, const char* strike, PutCall side) {
    auto base = base_for(venue, MarketType::Option, symbol, underlying);
    const auto ticks = *base.price_to_ticks(e9(strike));
    return OptionInstrument{.base = base, .strike = ticks, .expiry = expiry, .put_call = side};
}

} // namespace

TEST(OptionKey, StrikeIsInQuoteUnitsNotPremiumTicks) {
    const auto instrument = option(VenueId::Deribit, 1, "BTC", k_aug11, "64000", PutCall::Call);
    const auto key = key_of(std::get<OptionInstrument>(instrument));
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(key->strike, e9("64000"));
    EXPECT_EQ(key->underlying.view(), "BTC");
    EXPECT_EQ(canonical_name(*key), "BTC-20260811-64000-C");
}

TEST(OptionKey, AnOptionWithoutAnUnderlyingHasNoKey) {
    auto instrument = std::get<OptionInstrument>(option(VenueId::Okx, 1, "ETH", k_aug11, "3000", PutCall::Put));
    instrument.base.underlying = {};
    EXPECT_FALSE(key_of(instrument).has_value());
}

TEST(OptionKey, FractionalStrikesKeepTheirDigits) {
    const auto instrument = option(VenueId::Okx, 1, "SOL", k_aug12, "142.5", PutCall::Put);
    const auto key = key_of(std::get<OptionInstrument>(instrument));
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(canonical_name(*key), "SOL-20260812-142.5-P");
}

TEST(OptionChain, GroupsByUnderlyingExpiryAndStrikeWithTheCallPutPair) {
    std::vector<Instrument> catalogue{
        option(VenueId::Deribit, 1, "BTC", k_aug12, "64000", PutCall::Put),
        option(VenueId::Deribit, 2, "BTC", k_aug11, "66000", PutCall::Call),
        PerpInstrument{.base = base_for(VenueId::Deribit, MarketType::Perp, 3, "BTC")},
        option(VenueId::Deribit, 4, "ETH", k_aug11, "3000", PutCall::Call),
        option(VenueId::Deribit, 5, "BTC", k_aug11, "64000", PutCall::Put),
        option(VenueId::Deribit, 6, "BTC", k_aug11, "64000", PutCall::Call),
        option(VenueId::Deribit, 7, "BTC", k_aug11, "64000", PutCall::Call),
    };
    const auto chain = OptionChain::build(catalogue);
    EXPECT_EQ(chain.size(), 5U); // the perpetual and the second 64000 call are not placed

    ASSERT_EQ(chain.underlyings().size(), 2U);
    EXPECT_EQ(chain.underlyings()[0].underlying.view(), "BTC");
    const auto* btc = chain.underlying("BTC");
    ASSERT_NE(btc, nullptr);
    ASSERT_EQ(btc->expiries.size(), 2U);
    EXPECT_EQ(btc->expiries[0].expiry, k_aug11);

    const auto& near = btc->expiries[0].rows;
    ASSERT_EQ(near.size(), 2U);
    EXPECT_EQ(near[0].strike, e9("64000"));
    EXPECT_EQ(near[0].call->symbol().raw, 6U);
    EXPECT_EQ(near[0].put->symbol().raw, 5U);
    EXPECT_EQ(near[1].strike, e9("66000"));
    EXPECT_TRUE(near[1].call.has_value());
    EXPECT_FALSE(near[1].put.has_value());

    const OptionKey key{
        .underlying = *UnderlyingText::from("BTC"), .expiry = k_aug12, .strike = e9("64000"), .put_call = PutCall::Put};
    ASSERT_TRUE(chain.find(key).has_value());
    EXPECT_EQ(chain.find(key)->symbol().raw, 1U);
    EXPECT_FALSE(chain.find(OptionKey{key.underlying, key.expiry, key.strike, PutCall::Call}).has_value());
    EXPECT_EQ(chain.expiry("ETH", k_aug12), nullptr);
    EXPECT_EQ(chain.underlying("SOL"), nullptr);
}

TEST(OptionChain, MatchesTheSameContractAcrossVenuesByExactExpiry) {
    const std::vector<Instrument> deribit{
        option(VenueId::Deribit, 1, "ETH", k_aug11, "3000", PutCall::Call),
        option(VenueId::Deribit, 2, "ETH", k_aug11, "3000", PutCall::Put),
        option(VenueId::Deribit, 3, "ETH", k_aug11, "3100", PutCall::Call),
        option(VenueId::Deribit, 4, "ETH", k_aug12, "3000", PutCall::Call),
    };
    const std::vector<Instrument> derive{
        option(VenueId::Okx, 10, "ETH", k_aug11, "3000", PutCall::Call),
        option(VenueId::Okx, 11, "ETH", k_aug11, "3100", PutCall::Put),
        option(VenueId::Okx, 12, "ETH", k_aug12 + 3'600'000'000'000ULL, "3000", PutCall::Call),
    };
    const auto matched = match(OptionChain::build(deribit), OptionChain::build(derive));
    ASSERT_EQ(matched.size(), 1U);
    EXPECT_EQ(matched[0].left.venue(), VenueId::Deribit);
    EXPECT_EQ(matched[0].left.symbol().raw, 1U);
    EXPECT_EQ(matched[0].right.venue(), VenueId::Okx);
    EXPECT_EQ(matched[0].right.symbol().raw, 10U);
    EXPECT_EQ(canonical_name(matched[0].key), "ETH-20260811-3000-C");
}

TEST(OptionConvention, EachScaleConvertsExactlyAndUndefinedStaysUndefined) {
    EXPECT_EQ(to_fraction(e9("25.24"), IvScale::Percent), e9("0.2524"));
    EXPECT_EQ(to_fraction(e9("0.2524"), IvScale::Fraction), e9("0.2524"));
    EXPECT_EQ(to_per_vol_point(e9("891.855"), VegaScale::PerUnitVol), e9("8.91855"));
    EXPECT_EQ(to_per_day(e9("-365"), ThetaScale::PerYear), e9("-1"));
    EXPECT_TRUE(to_fraction(FixedE9::undef(), IvScale::Percent).is_undef());
    EXPECT_TRUE(to_per_day(FixedE9::undef(), ThetaScale::PerYear).is_undef());
}

TEST(OptionConvention, NormaliseLeavesAbsentFiguresUndefinedAndZeroAsZero) {
    const VenueGreeks venue{.delta = e9("0"),
                            .vega = e9("12"),
                            .theta = e9("-730"),
                            .mark_iv = e9("0.55"),
                            .mark_price = e9("101.5"),
                            .index_price = e9("3000.25")};
    const auto id = InstrumentId::make(VenueId::Okx, MarketType::Option, SymbolId{3});
    const auto out = normalise(
        id, venue, Convention{.vega = VegaScale::PerUnitVol, .theta = ThetaScale::PerYear}, UnixNanos{5}, UnixNanos{6});
    EXPECT_EQ(out.instrument, id);
    EXPECT_EQ(out.delta.raw, 0);
    EXPECT_TRUE(out.gamma.is_undef());
    EXPECT_TRUE(out.rho.is_undef());
    EXPECT_TRUE(out.bid_iv.is_undef());
    EXPECT_EQ(out.vega, e9("0.12"));
    EXPECT_EQ(out.theta, e9("-2"));
    EXPECT_EQ(out.mark_iv, e9("0.55"));
    EXPECT_EQ(out.mark_price, e9("101.5"));
    EXPECT_EQ(out.index_price, e9("3000.25"));
    EXPECT_EQ(out.ts_event.ns, 5U);
    EXPECT_EQ(out.ts_init.ns, 6U);
}
