#include <raikiri/optionrisk/risk.hpp>
#include <raikiri/options/pricing.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace raikiri;
using namespace raikiri::optionrisk;

namespace {

constexpr UnixNanos k_now{1'793'606'400'000'000'000ULL};
constexpr std::uint64_t k_day_ns = 86'400ULL * 1'000'000'000ULL;
constexpr UnixNanos k_expiry{k_now.ns + 365 * k_day_ns};

// by hand: F = K = 100000, σ = 0.2, T = 1, so d1 = 0.1, d2 = −0.1, N(0.1) = 0.539827837, φ(0.1) = 0.396952547
// C = P = F(2N(0.1) − 1) = 7965.5674; Δc = 0.5398278, Δp = −0.4601722; Γ = φ/(Fσ) = 1.984763e−5
// vega = Fφ/100 = 396.95255 per point; theta = −Fφσ/2/365 = −10.875412 per day
constexpr double k_price = 7965.567455;
constexpr double k_coin_price = 0.07965567455;
constexpr double k_call_delta = 0.539827837;
constexpr double k_put_delta = -0.460172163;
constexpr double k_gamma = 1.98476e-5;
constexpr double k_vega = 396.952547;
constexpr double k_theta = -10.875412;

FixedE9 e9(double value) {
    return FixedE9::from_raw(std::llround(value * 1e9));
}

double d(FixedE9 value) {
    return static_cast<double>(value.raw) / 1e9;
}

Holding holding(PutCall pc, double contracts, double entry, OptionUnit premium = OptionUnit::Quote, double size = 1.0) {
    static std::uint32_t symbol = 0;
    return {.account = "acct",
            .instrument = InstrumentId::make(VenueId::Deribit, MarketType::Option, SymbolId{++symbol}),
            .symbol = *SymbolText::from("BTC-OPT"),
            .key = {.underlying = *UnderlyingText::from("BTC"),
                    .expiry = k_expiry,
                    .strike = e9(100'000.0),
                    .put_call = pc},
            .premium = premium,
            .contracts = e9(contracts),
            .contract_value = e9(size),
            .entry = entry > 0.0 ? e9(entry) : FixedE9::undef(),
            .as_of = k_now};
}

MarketState market(OptionUnit premium) {
    return {.mark = premium == OptionUnit::Base ? e9(k_coin_price) : e9(k_price),
            .mark_from = MarkFrom::Venue,
            .mark_ts = k_now,
            .iv = e9(0.2),
            .greeks_ts = k_now,
            .forward = e9(100'000.0),
            .rate = e9(100'000.0)};
}

PositionRisk risk_of(const Holding& h) {
    return assess(h, market(h.premium), FixedE9::undef(), k_now, {});
}

} // namespace

TEST(Assess, LongCallInUsdByHand) {
    const auto p = risk_of(holding(PutCall::Call, 2.0, 5000.0));
    EXPECT_NEAR(d(p.units), 2.0, 1e-9);
    EXPECT_NEAR(d(p.days_to_expiry), 365.0, 1e-6);
    EXPECT_NEAR(d(p.vol), 0.2, 1e-6);
    EXPECT_NEAR(d(p.delta_coin), 2 * k_call_delta, 1e-6);
    EXPECT_NEAR(d(p.delta_usd), 2 * k_call_delta * 100'000.0, 0.1);
    EXPECT_NEAR(d(p.gamma), 2 * k_gamma, 1e-8);
    EXPECT_NEAR(d(p.gamma_usd_1pct), 2 * k_gamma * 1e10 / 100.0, 0.5);
    EXPECT_NEAR(d(p.vega_usd), 2 * k_vega, 1e-2);
    EXPECT_NEAR(d(p.theta_usd), 2 * k_theta, 1e-3);
    EXPECT_NEAR(d(p.premium_usd), 10'000.0, 1e-6);
    EXPECT_NEAR(d(p.value_usd), 2 * k_price, 1e-6);
    EXPECT_NEAR(d(p.unrealized_usd), 2 * k_price - 10'000.0, 1e-6);
    EXPECT_EQ(p.flags, 0U);
}

TEST(Assess, ShortCallMirrorsTheLong) {
    const auto long_call = risk_of(holding(PutCall::Call, 2.0, 5000.0));
    const auto short_call = risk_of(holding(PutCall::Call, -2.0, 5000.0));
    EXPECT_EQ(short_call.delta_coin, FixedE9::from_raw(-long_call.delta_coin.raw));
    EXPECT_EQ(short_call.gamma, FixedE9::from_raw(-long_call.gamma.raw));
    EXPECT_EQ(short_call.vega_usd, FixedE9::from_raw(-long_call.vega_usd.raw));
    EXPECT_GT(short_call.theta_usd.raw, 0);
    // received 10000, the position is worth −15931, so it is down by the same 5931 the long is up
    EXPECT_NEAR(d(short_call.premium_usd), -10'000.0, 1e-6);
    EXPECT_NEAR(d(short_call.value_usd), -2 * k_price, 1e-6);
    EXPECT_NEAR(d(short_call.unrealized_usd), 10'000.0 - 2 * k_price, 1e-6);
}

TEST(Assess, PutsLongAndShortByHand) {
    const auto long_put = risk_of(holding(PutCall::Put, 1.0, 4000.0));
    EXPECT_NEAR(d(long_put.delta_coin), k_put_delta, 1e-6);
    EXPECT_NEAR(d(long_put.gamma), k_gamma, 1e-8);
    EXPECT_NEAR(d(long_put.vega_usd), k_vega, 1e-2);
    EXPECT_NEAR(d(long_put.theta_usd), k_theta, 1e-3);
    EXPECT_NEAR(d(long_put.unrealized_usd), k_price - 4000.0, 1e-6);
    const auto short_put = risk_of(holding(PutCall::Put, -1.0, 4000.0));
    EXPECT_NEAR(d(short_put.delta_coin), -k_put_delta, 1e-6);
    EXPECT_NEAR(d(short_put.theta_usd), -k_theta, 1e-3);
    EXPECT_NEAR(d(short_put.premium_usd), -4000.0, 1e-6);
}

TEST(Assess, CoinAndUsdSettledAreOnePositionInUsd) {
    const auto usd = risk_of(holding(PutCall::Call, 2.0, 5000.0, OptionUnit::Quote));
    const auto coin = risk_of(holding(PutCall::Call, 2.0, 0.05, OptionUnit::Base));
    EXPECT_NEAR(d(coin.value), 2 * k_coin_price, 1e-8);
    EXPECT_NEAR(d(coin.value_usd), d(usd.value_usd), 1e-3);
    EXPECT_NEAR(d(coin.unrealized_usd), d(usd.unrealized_usd), 1e-3);
    EXPECT_NEAR(d(coin.delta_coin), d(usd.delta_coin), 1e-6);
    EXPECT_NEAR(d(coin.delta_usd), d(usd.delta_usd), 0.1);
    EXPECT_NEAR(d(coin.gamma), d(usd.gamma), 1e-8);
    EXPECT_NEAR(d(coin.vega_usd), d(usd.vega_usd), 1e-2);
    EXPECT_NEAR(d(coin.theta_usd), d(usd.theta_usd), 1e-3);
    // a coin premium is itself long the coin: Δ − C/F = 0.5398278 − 0.0796557 = 0.4601722 per unit
    EXPECT_NEAR(d(coin.delta_adjusted_coin), 2 * (k_call_delta - k_coin_price), 1e-6);
    EXPECT_EQ(usd.delta_adjusted_coin, usd.delta_coin);
}

TEST(Assess, ContractSizeScalesEveryFigure) {
    const auto one = risk_of(holding(PutCall::Call, 1.0, 5000.0));
    // ten contracts of 0.01 are 0.1 of the underlying, as on a venue with small contracts
    const auto small = risk_of(holding(PutCall::Call, 10.0, 5000.0, OptionUnit::Quote, 0.01));
    EXPECT_NEAR(d(small.units), 0.1, 1e-9);
    EXPECT_NEAR(d(small.delta_coin), 0.1 * d(one.delta_coin), 1e-7);
    EXPECT_NEAR(d(small.value_usd), 0.1 * k_price, 1e-6);
    EXPECT_NEAR(d(small.premium_usd), 500.0, 1e-6);
}

TEST(Assess, MissingAndOldInputsAreFlaggedNotZeroed) {
    auto no_mark = market(OptionUnit::Quote);
    no_mark.mark = FixedE9::undef();
    no_mark.mark_from = MarkFrom::None;
    no_mark.iv = FixedE9::undef();
    const auto blind = assess(holding(PutCall::Call, 1.0, 5000.0), no_mark, FixedE9::undef(), k_now, {});
    EXPECT_NE(blind.flags & k_mark_missing, 0U);
    EXPECT_NE(blind.flags & k_greeks_missing, 0U);
    EXPECT_TRUE(blind.value_usd.is_undef());
    EXPECT_TRUE(blind.delta_coin.is_undef());
    EXPECT_TRUE(blind.unrealized_usd.is_undef());

    auto iv_only = no_mark;
    iv_only.iv = e9(0.2);
    iv_only.greeks_ts = k_now;
    const auto modelled = assess(holding(PutCall::Call, 1.0, 5000.0), iv_only, FixedE9::undef(), k_now, {});
    EXPECT_NE(modelled.flags & k_mark_model, 0U);
    EXPECT_EQ(modelled.flags & k_mark_missing, 0U);
    EXPECT_NEAR(d(modelled.value_usd), k_price, 1e-3);

    const UnixNanos later{k_now.ns + 120'000'000'000ULL};
    const auto old = assess(
        holding(PutCall::Call, 1.0, 5000.0), market(OptionUnit::Quote), FixedE9::undef(), later, {});
    EXPECT_NE(old.flags & k_mark_stale, 0U);
    EXPECT_NE(old.flags & k_greeks_stale, 0U);
    EXPECT_NE(old.flags & k_position_stale, 0U);

    const auto no_entry = risk_of(holding(PutCall::Call, 1.0, 0.0));
    EXPECT_NE(no_entry.flags & k_entry_missing, 0U);
    EXPECT_TRUE(no_entry.unrealized_usd.is_undef());
    EXPECT_FALSE(no_entry.value_usd.is_undef());

    auto no_rate = market(OptionUnit::Base);
    no_rate.rate = FixedE9::undef();
    const auto coinless = assess(
        holding(PutCall::Call, 1.0, 0.05, OptionUnit::Base), no_rate, FixedE9::undef(), k_now, {});
    EXPECT_NE(coinless.flags & k_rate_missing, 0U);
    EXPECT_FALSE(coinless.value.is_undef());
    EXPECT_TRUE(coinless.value_usd.is_undef());
}

TEST(MarketOf, PrefersTheVenueAndFallsBackToTheMid) {
    optiondata::OptionQuote q{.bid = e9(90.0), .ask = e9(110.0), .ts = UnixNanos{5}};
    auto plain = market_of(q, e9(1000.0), e9(990.0));
    EXPECT_EQ(plain.mark, e9(100.0));
    EXPECT_EQ(plain.mark_from, MarkFrom::Mid);
    EXPECT_EQ(plain.forward, e9(1000.0));
    EXPECT_EQ(plain.rate, e9(990.0));

    OptionGreeks g{};
    g.mark_price = e9(101.0);
    g.mark_iv = e9(0.5);
    g.underlying_price = e9(1010.0);
    g.index_price = e9(1005.0);
    g.ts_event = UnixNanos{7};
    q.greeks = g;
    const auto venue = market_of(q, e9(1000.0), e9(990.0));
    EXPECT_EQ(venue.mark, e9(101.0));
    EXPECT_EQ(venue.mark_from, MarkFrom::Venue);
    EXPECT_EQ(venue.forward, e9(1010.0));
    EXPECT_EQ(venue.rate, e9(1005.0));
    EXPECT_EQ(venue.greeks_ts, UnixNanos{7});

    q.greeks = std::nullopt;
    q.ask = FixedE9::undef();
    EXPECT_EQ(market_of(q, e9(1000.0), e9(990.0)).mark_from, MarkFrom::None);
}

TEST(Aggregate, SignsAcrossAStraddleAndASynthetic) {
    // long call and short put of one strike is long the forward: delta 1, no gamma, vega or theta
    const std::vector positions{risk_of(holding(PutCall::Call, 1.0, 5000.0)),
                                risk_of(holding(PutCall::Put, -1.0, 5000.0))};
    const auto synthetic = aggregate(positions, {}, {}, k_now);
    ASSERT_EQ(synthetic.by_underlying.size(), 1U);
    const auto& t = synthetic.by_underlying[0].totals;
    EXPECT_EQ(t.positions, 2U);
    EXPECT_EQ(t.incomplete, 0U);
    EXPECT_NEAR(d(t.delta_coin), 1.0, 1e-6);
    EXPECT_NEAR(d(t.delta_usd), 100'000.0, 0.1);
    EXPECT_NEAR(d(t.gamma), 0.0, 1e-8);
    EXPECT_NEAR(d(t.vega_usd), 0.0, 1e-2);
    EXPECT_NEAR(d(t.theta_usd), 0.0, 1e-3);
    // call paid 5000, put received 5000; worth C − P = 0 at the money forward
    EXPECT_NEAR(d(t.premium_usd), 0.0, 1e-6);
    EXPECT_NEAR(d(t.value_usd), 0.0, 1e-3);

    // long both: delta 2Δc − 1 ≈ 0.0796557, gamma, vega and theta double
    const std::vector straddle{risk_of(holding(PutCall::Call, 1.0, 5000.0)),
                               risk_of(holding(PutCall::Put, 1.0, 5000.0))};
    const auto both = aggregate(straddle, {}, {}, k_now);
    EXPECT_NEAR(d(both.total.delta_coin), k_call_delta + k_put_delta, 1e-6);
    EXPECT_NEAR(d(both.total.vega_usd), 2 * k_vega, 1e-2);
    EXPECT_NEAR(d(both.total.theta_usd), 2 * k_theta, 1e-3);
    ASSERT_EQ(both.by_expiry.size(), 1U);
    EXPECT_EQ(both.by_expiry[0].expiry, k_expiry);
    EXPECT_NEAR(d(both.by_expiry[0].days_to_expiry), 365.0, 1e-6);
    EXPECT_EQ(both.by_expiry[0].totals.positions, 2U);
}

TEST(Aggregate, KeepsUnderlyingsAndExpiriesApartAndCountsWhatItCouldNotSum) {
    auto eth = holding(PutCall::Call, 1.0, 5000.0);
    eth.key.underlying = *UnderlyingText::from("ETH");
    auto near = holding(PutCall::Call, 1.0, 5000.0);
    near.key.expiry = UnixNanos{k_now.ns + 30 * k_day_ns};
    auto blind = market(OptionUnit::Quote);
    blind.mark = FixedE9::undef();
    blind.mark_from = MarkFrom::None;
    blind.iv = FixedE9::undef();
    const std::vector positions{risk_of(holding(PutCall::Call, 1.0, 5000.0)),
                                risk_of(eth),
                                risk_of(near),
                                assess(holding(PutCall::Call, 1.0, 5000.0), blind, FixedE9::undef(), k_now, {})};
    const auto out = aggregate(positions, {}, {}, k_now);
    ASSERT_EQ(out.by_underlying.size(), 2U);
    EXPECT_EQ(out.by_underlying[0].underlying.view(), "BTC");
    EXPECT_EQ(out.by_underlying[0].totals.positions, 3U);
    EXPECT_EQ(out.by_underlying[0].totals.incomplete, 1U);
    EXPECT_EQ(out.by_underlying[1].totals.incomplete, 0U);
    ASSERT_EQ(out.by_expiry.size(), 3U);
    EXPECT_EQ(out.by_expiry[0].expiry, near.key.expiry);
    EXPECT_EQ(out.total.positions, 4U);
    EXPECT_EQ(out.total.incomplete, 1U);
    EXPECT_NEAR(d(out.by_underlying[1].totals.value_usd), k_price, 1e-6);
}

TEST(Scenario, ZeroShockIsTheCurrentValueInEveryCellOfThatRow) {
    const std::vector positions{risk_of(holding(PutCall::Call, 2.0, 5000.0)),
                                risk_of(holding(PutCall::Put, -1.0, 0.04, OptionUnit::Base)),
                                risk_of(holding(PutCall::Call, -3.0, 4000.0))};
    FixedE9 current = FixedE9::from_raw(0);
    for (const auto& p : positions) {
        current = current + p.value_usd;
    }
    const std::vector spot{e9(-0.1), e9(0.0), e9(0.1)};
    const std::vector vol{e9(-0.05), e9(0.0), e9(0.05)};
    const auto m = scenario(positions, spot, vol);
    ASSERT_EQ(m.cells.size(), 9U);
    EXPECT_EQ(m.base_value_usd, current);
    const auto& centre = m.cells[4];
    EXPECT_EQ(centre.spot, e9(0.0));
    EXPECT_EQ(centre.vol, e9(0.0));
    EXPECT_EQ(centre.value_usd, current);
    EXPECT_EQ(centre.pnl_usd.raw, 0);
}

TEST(Scenario, RevaluesInFullRatherThanByGreeks) {
    const std::vector positions{risk_of(holding(PutCall::Call, 1.0, 5000.0))};
    const std::vector spot{e9(0.1)};
    const std::vector vol{e9(0.0), e9(0.1)};
    const auto m = scenario(positions, spot, vol);
    // by hand: d1 = ln 1.1 / 0.2 + 0.1 = 0.57655, so C(110000) = 14292.01, and at σ = 0.3, 18141.01;
    // delta and gamma alone would say 7965.57 + 5398.28 + 992.38 = 14356.23
    constexpr double k_up = 14'292.011;
    constexpr double k_up_vol = 18'141.012;
    ASSERT_EQ(m.cells.size(), 2U);
    EXPECT_NEAR(d(m.cells[0].value_usd), k_up, 1e-2);
    EXPECT_NEAR(d(m.cells[0].pnl_usd), k_up - k_price, 1e-2);
    EXPECT_NEAR(d(m.cells[1].value_usd), k_up_vol, 1e-2);
}

TEST(Scenario, ACoinPremiumMovesWithTheCoin) {
    const std::vector spot{e9(0.1)};
    const std::vector vol{e9(0.0)};
    const std::vector usd{risk_of(holding(PutCall::Call, 1.0, 5000.0))};
    const std::vector coin{risk_of(holding(PutCall::Call, 1.0, 0.05, OptionUnit::Base))};
    const auto a = scenario(usd, spot, vol);
    const auto b = scenario(coin, spot, vol);
    // F moves and the coin it pays in moves with it, so a coin contract's usd value is the usd contract's
    EXPECT_NEAR(d(a.cells[0].value_usd), d(b.cells[0].value_usd), 1e-2);
}

TEST(Scenario, MarksTheWorstCellAndExcludesWhatItCannotPrice) {
    auto blind = market(OptionUnit::Quote);
    blind.mark = FixedE9::undef();
    blind.mark_from = MarkFrom::None;
    blind.iv = FixedE9::undef();
    const auto excluded = holding(PutCall::Call, 5.0, 5000.0);
    // short a straddle loses most with a large move and more vol
    const std::vector positions{risk_of(holding(PutCall::Call, -1.0, 5000.0)),
                                risk_of(holding(PutCall::Put, -1.0, 5000.0)),
                                assess(excluded, blind, FixedE9::undef(), k_now, {})};
    const std::vector spot{e9(-0.3), e9(0.0), e9(0.3)};
    const std::vector vol{e9(-0.1), e9(0.0), e9(0.2)};
    const auto m = scenario(positions, spot, vol);
    ASSERT_TRUE(m.worst.has_value());
    const auto& worst = m.cells[*m.worst];
    EXPECT_EQ(worst.vol, e9(0.2));
    EXPECT_EQ(worst.spot, e9(0.3));
    for (const auto& cell : m.cells) {
        EXPECT_GE(cell.pnl_usd, worst.pnl_usd);
    }
    EXPECT_EQ(m.positions, 2U);
    ASSERT_EQ(m.excluded.size(), 1U);
    EXPECT_EQ(m.excluded[0], excluded.instrument);
}

TEST(Scenario, AVolShockPastZeroFloorsAndStillPrices) {
    const std::vector positions{risk_of(holding(PutCall::Call, 1.0, 5000.0))};
    const std::vector spot{e9(0.0)};
    const std::vector vol{e9(-0.5)};
    const auto m = scenario(positions, spot, vol);
    ASSERT_FALSE(m.cells[0].value_usd.is_undef());
    // at a 1% vol an at the money call is worth about 0.4·F·σ = 399 and the 7965 mark loses most of itself
    EXPECT_NEAR(d(m.cells[0].value_usd), 398.9, 1.0);
}

TEST(Settle, LongAndShortAtTheSettlementPriceByHand) {
    const SettlementPrice at{.price = e9(110'000.0), .by = SettledBy::Venue, .ts = k_expiry};
    const auto call = settle(holding(PutCall::Call, 2.0, 5000.0), at, k_expiry, false);
    EXPECT_NEAR(d(call.payoff), 10'000.0, 1e-9);
    EXPECT_NEAR(d(call.proceeds), 20'000.0, 1e-9);
    EXPECT_NEAR(d(call.realized), 10'000.0, 1e-9);
    EXPECT_NEAR(d(call.realized_usd), 10'000.0, 1e-9);
    EXPECT_FALSE(call.late);

    // a short put out of the money keeps what it received
    const auto put = settle(holding(PutCall::Put, -1.0, 3000.0), at, k_expiry, false);
    EXPECT_NEAR(d(put.payoff), 0.0, 1e-9);
    EXPECT_NEAR(d(put.realized), 3000.0, 1e-9);

    // a short call in the money pays the payoff less what it received
    const auto short_call = settle(holding(PutCall::Call, -1.0, 3000.0), at, k_expiry, false);
    EXPECT_NEAR(d(short_call.proceeds), -10'000.0, 1e-9);
    EXPECT_NEAR(d(short_call.realized), -7000.0, 1e-9);

    const SettlementPrice low{.price = e9(90'000.0), .by = SettledBy::Venue, .ts = k_expiry};
    const auto long_put = settle(holding(PutCall::Put, 1.0, 3000.0), low, k_expiry, false);
    EXPECT_NEAR(d(long_put.realized), 7000.0, 1e-9);
}

TEST(Settle, ACoinContractPaysIntrinsicOverThePriceInCoin) {
    const SettlementPrice at{.price = e9(125'000.0), .by = SettledBy::Index, .ts = k_expiry};
    const auto call = settle(holding(PutCall::Call, 2.0, 0.05, OptionUnit::Base), at, k_expiry, false);
    // 25000 / 125000 = 0.2 coin per unit; 2 × (0.2 − 0.05) = 0.3 coin = 37500 usd at the settlement price
    EXPECT_NEAR(d(call.payoff), 0.2, 1e-9);
    EXPECT_NEAR(d(call.realized), 0.3, 1e-9);
    EXPECT_NEAR(d(call.realized_usd), 37'500.0, 1e-6);
    EXPECT_NEAR(d(call.proceeds_usd), 50'000.0, 1e-6);
}

TEST(Settle, FlagsALateIndexAndKeepsPayoffWithoutAnEntry) {
    const SettlementPrice late{
        .price = e9(110'000.0), .by = SettledBy::Index, .ts = UnixNanos{k_expiry.ns + k_settle_late_ns + 1}};
    const auto one = settle(holding(PutCall::Call, 1.0, 0.0), late, late.ts, true);
    EXPECT_TRUE(one.late);
    EXPECT_TRUE(one.from_last_seen);
    EXPECT_NEAR(d(one.proceeds), 10'000.0, 1e-9);
    EXPECT_TRUE(one.realized.is_undef());
}

TEST(Aggregate, SettledAndBookedRealizedLandOnTheirExpiry) {
    const SettlementPrice at{.price = e9(110'000.0), .by = SettledBy::Venue, .ts = k_expiry};
    const std::vector settled{settle(holding(PutCall::Call, 2.0, 5000.0), at, k_expiry, false)};
    const std::vector booked{BookedRealized{.key = holding(PutCall::Put, 1.0, 1.0).key, .realized_usd = e9(250.0)}};
    const auto out = aggregate({}, settled, booked, k_now);
    EXPECT_NEAR(d(out.total.realized_usd), 10'250.0, 1e-6);
    EXPECT_EQ(out.total.settled, 1U);
    EXPECT_EQ(out.total.positions, 0U);
    ASSERT_EQ(out.by_expiry.size(), 1U);
    EXPECT_NEAR(d(out.by_expiry[0].totals.realized_usd), 10'250.0, 1e-6);
}
