#include <raikiri/options/pricing.hpp>

#include <gtest/gtest.h>

#include <lets_be_rational.h>

#include <cfloat>
#include <cmath>
#include <random>
#include <vector>

namespace raikiri::options {
namespace {

FixedE9 e9(double value) {
    return FixedE9::from_raw(std::llround(value * 1e9));
}

double num(FixedE9 value) {
    return static_cast<double>(value.raw) / 1e9;
}

constexpr std::uint64_t k_ns_per_second = 1'000'000'000ULL;

UnixNanos at_seconds(std::uint64_t seconds) {
    return UnixNanos{seconds * k_ns_per_second};
}

UnixNanos at_millis(std::uint64_t millis) {
    return UnixNanos{millis * 1'000'000ULL};
}

// Hull, Options, Futures and Other Derivatives, example 15.6: S 42, K 40, r 10%, sigma 20%, T 0.5
TEST(Black76, HullExample15Point6OnTheForward) {
    const double r = 0.10;
    const double t = 0.5;
    const Black76Input call{.forward = e9(42.0 * std::exp(r * t)),
                            .strike = e9(40.0),
                            .vol = e9(0.20),
                            .years = e9(t),
                            .discount = e9(std::exp(-r * t))};
    auto put = call;
    put.put_call = PutCall::Put;
    // the book prints four places
    EXPECT_NEAR(num(black76(call).price), 4.7594, 5e-5);
    EXPECT_NEAR(num(black76(put).price), 0.8086, 5e-5);
    EXPECT_NEAR(num(black76(call).price), 4.759422392871536, 1e-7);
    EXPECT_NEAR(num(black76(put).price), 0.8085993729000926, 1e-7);
}

// Hull, chapter 19: S 49, K 50, r 5%, sigma 20%, T 0.3846; the book's greeks are on the spot
TEST(Black76, HullChapter19GreeksOnceMovedToTheSpot) {
    const double s = 49.0;
    const double r = 0.05;
    const double t = 0.3846;
    const double forward = s * std::exp(r * t);
    const auto g = black76({.forward = e9(forward),
                            .strike = e9(50.0),
                            .vol = e9(0.20),
                            .years = e9(t),
                            .discount = e9(std::exp(-r * t))});
    const double growth = forward / s;
    EXPECT_NEAR(num(g.delta) * growth, 0.5216, 5e-5);
    EXPECT_NEAR(num(g.gamma) * growth * growth, 0.0655, 5e-5);
    EXPECT_NEAR(num(g.vega) * 100.0, 12.105, 5e-4);
    // the book holds the spot, this holds the forward, which drifts at r under a held spot
    const double theta_spot = num(g.theta) * 365.0 - r * forward * num(g.delta);
    EXPECT_NEAR(theta_spot, -4.305, 5e-4);
}

// Haug, The Complete Guide to Option Pricing Formulas, Black-76: F = K = 19, r 10%, sigma 28%, T 0.75
TEST(Black76, HaugAtTheForwardCallEqualsPut) {
    const Black76Input call{
        .forward = e9(19.0), .strike = e9(19.0), .vol = e9(0.28), .years = e9(0.75), .discount = e9(std::exp(-0.075))};
    auto put = call;
    put.put_call = PutCall::Put;
    EXPECT_NEAR(num(black76(call).price), 1.7011, 5e-5);
    EXPECT_EQ(black76(call).price, black76(put).price);
}

TEST(Black76, PutCallParityAndPutDelta) {
    for (const double strike : {60.0, 95.0, 100.0, 140.0}) {
        const Black76Input call{
            .forward = e9(100.0), .strike = e9(strike), .vol = e9(0.6), .years = e9(0.1), .discount = e9(0.99)};
        auto put = call;
        put.put_call = PutCall::Put;
        const auto c = black76(call);
        const auto p = black76(put);
        EXPECT_NEAR(num(c.price) - num(p.price), 0.99 * (100.0 - strike), 2e-9) << strike;
        EXPECT_NEAR(num(c.delta) - num(p.delta), 0.99, 2e-9) << strike;
        EXPECT_EQ(c.gamma, p.gamma);
        EXPECT_EQ(c.vega, p.vega);
    }
}

TEST(Black76, RefusesWhatItCannotPrice) {
    const Black76Input good{.forward = e9(100.0), .strike = e9(100.0), .vol = e9(0.5), .years = e9(0.25)};
    EXPECT_FALSE(black76(good).price.is_undef());
    std::vector<Black76Input> bad(5, good);
    bad[0].forward = FixedE9::undef();
    bad[1].strike = e9(0.0);
    bad[2].years = e9(0.0);
    bad[3].vol = e9(0.00001);
    bad[4].vol = e9(11.0);
    for (const auto& in : bad) {
        const auto g = black76(in);
        EXPECT_TRUE(g.price.is_undef() && g.delta.is_undef() && g.gamma.is_undef() && g.vega.is_undef() &&
                    g.theta.is_undef());
    }
}

// tests/venues/deribit/fixtures/ws_ticker_option.json: BTC-11AUG26-64000-C, greeks in usd, premium in coin
TEST(Black76, DeribitFixtureGreeksAndCoinMark) {
    const auto years = years_between(at_millis(1786395411520ULL), at_seconds(1786435200ULL));
    const Black76Input in{.forward = e9(64107.29), .strike = e9(64000.0), .vol = e9(0.2524), .years = years};
    const auto usd = black76(in);
    EXPECT_NEAR(num(usd.delta), 0.57588, 5e-5);
    EXPECT_NEAR(num(usd.gamma), 6.8e-4, 5e-6); // the venue prints two figures
    EXPECT_NEAR(num(usd.vega), 8.91855, 2e-3);
    // mark_price 0.0044 on a 0.0001 tick
    EXPECT_NEAR(num(inverse(in).price), 0.0044, 1e-4);

    // under a day out the venue reports theta over the time left: the per-day figure times the days left
    const double days_left = num(years) * 365.0;
    ASSERT_LT(days_left, 1.0);
    EXPECT_NEAR(num(usd.theta), -244.43, 0.05);
    EXPECT_NEAR(num(usd.theta) * days_left, -112.53218, 0.1);
}

struct OkxRow {
    PutCall put_call{PutCall::Call};
    double strike{0};
    double forward{0};
    double vol{0};
    std::uint64_t expiry_s{0};
    double delta_pa{0};
    double gamma_pa{0};
    double vega_pa{0};
    double theta_pa{0};
    double delta_bs{0};
    double gamma_bs{0};
    double vega_bs{0};
    double theta_bs{0};
};

// tests/venues/okx/fixtures/ws_opt_summary.json; the *BS figures are usd, the bare ones price-adjusted coin
TEST(Inverse, OkxFixturePriceAdjustedGreeks) {
    const std::vector<OkxRow> rows{
        {PutCall::Put,
         75000,
         84907.8657133353,
         0.4733926174,
         1791532800ULL,
         -0.0162891307,
         0.7134050811,
         0.0000494363,
         -0.0002089957,
         -0.0159486245,
         0.0000080182,
         4.1976362412,
         -17.7517234756},
        {PutCall::Call,
         80000,
         84873.8193048651,
         0.516556701,
         1791100800ULL,
         0.94027248,
         -1.5400886831,
         0.0000028842,
         -0.000073553,
         0.9977099466,
         0.0000040096,
         0.2449051857,
         27.6875099929},
        {PutCall::Call,
         86500,
         84907.8657133353,
         0.2810016875,
         1791532800ULL,
         0.2961502484,
         9.4384042614,
         0.0004323432,
         -0.0010848112,
         0.3027372349,
         0.0001181335,
         36.7102244855,
         -92.0036095151},
    };
    const auto now = at_millis(1791049079993ULL);
    for (const auto& row : rows) {
        const Black76Input in{.forward = e9(row.forward),
                              .strike = e9(row.strike),
                              .vol = e9(row.vol),
                              .years = years_between(now, at_seconds(row.expiry_s)),
                              .put_call = row.put_call};
        const auto usd = black76(in);
        const auto coin = inverse(in);
        // the venue's stamp and ours differ by its compute lag, a few parts in ten thousand
        const auto rel = [](double got, double want) { return std::fabs(got - want) / std::fabs(want); };
        EXPECT_LT(rel(num(usd.delta), row.delta_bs), 5e-4) << row.strike;
        EXPECT_LT(rel(num(usd.gamma), row.gamma_bs), 5e-4) << row.strike;
        EXPECT_LT(rel(num(usd.vega), row.vega_bs), 5e-4) << row.strike;
        EXPECT_LT(rel(num(coin.delta), row.delta_pa), 5e-4) << row.strike;
        EXPECT_LT(rel(num(coin.gamma), row.gamma_pa), 5e-4) << row.strike;
        EXPECT_LT(rel(num(coin.vega), row.vega_pa), 1e-3) << row.strike; // e9 leaves four figures here
        // the venue's own theta under a day out follows neither formula
        if (num(in.years) * 365.0 > 1.0) {
            EXPECT_LT(rel(num(usd.theta), row.theta_bs), 2e-3) << row.strike;
            EXPECT_LT(rel(num(coin.theta), row.theta_pa), 2e-3) << row.strike;
        }
        EXPECT_NEAR(num(usd.delta) - num(coin.delta), num(coin.price), 2e-9) << "delta_pa = delta - C/F";
    }
}

TEST(ImpliedVol, RoundTripsThroughFixedPoint) {
    for (const double vol : {0.05, 0.25, 0.8, 2.0}) {
        for (const double moneyness : {0.5, 0.9, 1.0, 1.1, 2.0}) {
            for (const double years : {1.0 / 365.0, 30.0 / 365.0, 2.0}) {
                for (const auto pc : {PutCall::Call, PutCall::Put}) {
                    const Black76Input in{.forward = e9(100.0),
                                          .strike = e9(100.0 * moneyness),
                                          .vol = e9(vol),
                                          .years = e9(years),
                                          .put_call = pc,
                                          .discount = e9(0.98)};
                    const auto g = black76(in);
                    const double vega_unit = num(g.vega) * 100.0;
                    if (vega_unit < 1e-3) {
                        continue; // a price of a few e9 steps carries no vol
                    }
                    const double tolerance = 1e-9 * (2.0 + 1.0 / vega_unit);
                    const auto back = implied_vol({.price = g.price,
                                                   .forward = in.forward,
                                                   .strike = in.strike,
                                                   .years = in.years,
                                                   .put_call = pc,
                                                   .discount = in.discount});
                    ASSERT_FALSE(back.is_undef()) << vol << " " << moneyness << " " << years;
                    EXPECT_NEAR(num(back), vol, tolerance) << vol << " " << moneyness << " " << years;
                }
            }
        }
    }
}

TEST(ImpliedVol, ReadsACoinPremiumAgainstTheForward) {
    const Black76Input in{.forward = e9(64107.29), .strike = e9(66000.0), .vol = e9(0.45), .years = e9(0.05)};
    const auto coin = inverse(in);
    const auto back = implied_vol(
        {.price = coin.price, .forward = in.forward, .strike = in.strike, .years = in.years, .unit = OptionUnit::Base});
    // a coin premium in e9 is a usd price to about 6e-5
    EXPECT_NEAR(num(back), 0.45, 1e-5);
}

TEST(ImpliedVol, UndefOutsideTheArbitrageBounds) {
    const PremiumInput itm{.price = e9(10.0), .forward = e9(110.0), .strike = e9(100.0), .years = e9(0.1)};
    EXPECT_TRUE(implied_vol(itm).is_undef()) << "exactly intrinsic";
    auto below = itm;
    below.price = e9(9.0);
    EXPECT_TRUE(implied_vol(below).is_undef());
    auto above = itm;
    above.price = e9(110.0);
    EXPECT_TRUE(implied_vol(above).is_undef()) << "a call is worth less than the forward";
    auto zero = itm;
    zero.price = e9(0.0);
    EXPECT_TRUE(implied_vol(zero).is_undef());
}

// the paper's domain, in doubles against the vendored code: |ln(F/K)| <= 5 and sigma*sqrt(T) in [1e-4, 5]
TEST(ImpliedVol, JaeckelDomainRoundTripToMachinePrecision) {
    int checked = 0;
    for (const double x : {-5.0, -2.0, -0.5, -1e-3, 0.0, 1e-3, 0.5, 2.0, 5.0}) {
        for (double s = 1e-4; s <= 5.0; s *= 1.25) {
            for (const double q : {1.0, -1.0}) {
                const double forward = 1.0;
                const double strike = std::exp(-x);
                const double price = Black(forward, strike, s, 1.0, q);
                const double intrinsic = std::fmax(q * (forward - strike), 0.0);
                // prices too small to resolve, or indistinguishable from intrinsic or the bound, carry no vol
                if (price < DBL_MIN || price - intrinsic <= 1e-12 * price || price >= (q > 0 ? forward : strike)) {
                    continue;
                }
                const double back = ImpliedBlackVolatility(price, forward, strike, 1.0, q);
                const double attainable = ImpliedVolatilityAttainableAccuracy(x, s, q);
                const double error = std::fabs(back - s) / s;
                ++checked;
                // where the price cannot resolve 1e-13 of vol, within twice what it can resolve
                EXPECT_LT(error, std::fmax(1e-13, 2.0 * attainable)) << "x " << x << " s " << s << " q " << q;
            }
        }
    }
    EXPECT_GT(checked, 300);
}

TEST(Forward, ParityRecoversTheForwardBothWays) {
    std::vector<ParityQuote> usd;
    std::vector<ParityQuote> coin;
    for (double strike = 80.0; strike <= 120.0; strike += 5.0) {
        Black76Input in{.forward = e9(101.3), .strike = e9(strike), .vol = e9(0.6), .years = e9(0.08)};
        // coin-settled books price at no discount, as the venues do
        const auto c_coin = inverse(in);
        in.discount = e9(0.995);
        const auto c = black76(in);
        in.put_call = PutCall::Put;
        const auto p = black76(in);
        in.discount = k_unit_e9;
        const auto p_coin = inverse(in);
        usd.push_back({.strike = in.strike, .call = c.price, .put = p.price});
        coin.push_back({.strike = in.strike, .call = c_coin.price, .put = p_coin.price});
    }
    const auto from_usd = forward_from_parity(usd, OptionUnit::Quote, e9(0.995));
    ASSERT_TRUE(from_usd.has_value());
    EXPECT_NEAR(num(*from_usd), 101.3, 1e-8);
    const auto from_coin = forward_from_parity(coin, OptionUnit::Base);
    ASSERT_TRUE(from_coin.has_value());
    // a coin premium at e9 is about four usd digits fewer
    EXPECT_NEAR(num(*from_coin), 101.3, 1e-5);

    // one broken pair at the money moves the median by nothing
    usd[4].call = e9(num(usd[4].call) + 3.0);
    EXPECT_NEAR(num(*forward_from_parity(usd, OptionUnit::Quote, e9(0.995))), 101.3, 1e-8);
    EXPECT_FALSE(forward_from_parity({}, OptionUnit::Quote).has_value());
}

TEST(Forward, FallsBackFromParityToFutureToPerp) {
    EXPECT_EQ(pick_forward({.parity = e9(100.0), .future = e9(101.0), .perp = e9(99.0)}).source, ForwardSource::Parity);
    const auto future = pick_forward({.future = e9(101.0), .perp = e9(99.0)});
    EXPECT_EQ(future.source, ForwardSource::Future);
    EXPECT_EQ(future.value, e9(101.0));
    const auto perp = pick_forward({.perp = e9(100.0), .carry = e9(0.002)});
    EXPECT_EQ(perp.source, ForwardSource::Perp);
    EXPECT_EQ(perp.value, e9(100.2));
    EXPECT_EQ(pick_forward({}).source, ForwardSource::None);
    EXPECT_TRUE(pick_forward({}).value.is_undef());
}

TEST(Forward, YearsAreAct365) {
    EXPECT_EQ(years_between(at_seconds(0), at_seconds(365ULL * 86'400ULL)), e9(1.0));
    EXPECT_EQ(years_between(at_seconds(0), at_seconds(73ULL * 86'400ULL)), e9(0.2));
    EXPECT_TRUE(years_between(at_seconds(5), at_seconds(5)).is_undef());
}

TEST(Smile, InterpolatesInLogStrikeAndTakesTheLocalSlope) {
    const std::vector<SmilePoint> smile{{.strike = e9(90.0), .vol = e9(0.60)},
                                        {.strike = e9(100.0), .vol = e9(0.50)},
                                        {.strike = e9(110.0), .vol = e9(0.46)}};
    const auto listed = smile_at(smile, e9(100.0));
    ASSERT_TRUE(listed.has_value());
    EXPECT_EQ(listed->vol, e9(0.50));
    EXPECT_NEAR(num(listed->skew), (0.46 - 0.60) / std::log(110.0 / 90.0), 2e-9);

    const auto between = smile_at(smile, e9(105.0));
    ASSERT_TRUE(between.has_value());
    const double w = std::log(1.05) / std::log(1.1);
    EXPECT_NEAR(num(between->vol), 0.50 + w * (0.46 - 0.50), 2e-9);
    EXPECT_NEAR(num(between->skew), (0.46 - 0.50) / std::log(1.1), 2e-9);

    EXPECT_FALSE(smile_at(smile, e9(80.0)).has_value()) << "no extrapolation";
    EXPECT_FALSE(smile_at(std::vector<SmilePoint>{smile[1], smile[0]}, e9(95.0)).has_value()) << "unsorted";
}

// the figures the research note quotes: F 100k, K 105k, sigma 45%, dsigma/dlnK -0.2
TEST(Digital, SkewTermMatchesTheNoteAndTheCallSpread) {
    const auto week = digital({.forward = e9(100000.0),
                               .strike = e9(105000.0),
                               .years = e9(7.0 / 365.0),
                               .smile = {.vol = e9(0.45), .skew = e9(-0.2)}});
    EXPECT_NEAR(num(week.flat), 0.2078, 5e-5);
    EXPECT_NEAR(num(week.skewed), 0.2157, 5e-5);
    const auto month = digital({.forward = e9(100000.0),
                                .strike = e9(105000.0),
                                .years = e9(30.0 / 365.0),
                                .smile = {.vol = e9(0.45), .skew = e9(-0.2)}});
    EXPECT_NEAR(num(month.flat), 0.3290, 5e-5);
    EXPECT_NEAR(num(month.skewed), 0.3497, 5e-5);

    // -dC/dK on a smile linear in ln K; the central difference is O(h^2)
    const auto call = [](double strike) {
        const double vol = 0.45 - 0.2 * std::log(strike / 100000.0);
        return num(
            black76({.forward = e9(100000.0), .strike = e9(strike), .vol = e9(vol), .years = e9(30.0 / 365.0)}).price);
    };
    const auto at = digital({.forward = e9(100000.0),
                             .strike = e9(105000.0),
                             .years = e9(30.0 / 365.0),
                             .smile = {.vol = e9(0.45 - 0.2 * std::log(1.05)), .skew = e9(-0.2)}});
    const double h = 50.0;
    EXPECT_NEAR(num(at.skewed), (call(105000.0 - h) - call(105000.0 + h)) / (2.0 * h), 2e-5);
}

TEST(Touch, ClosedFormBoundsAndTheTwiceDigitalCrossCheck) {
    const auto flat = [](double barrier, double vol, double days) {
        return one_touch({.forward = e9(100.0),
                          .barrier = e9(barrier),
                          .years = e9(days / 365.0),
                          .smile = {.vol = e9(vol), .skew = e9(0.0)}});
    };
    const auto up = flat(110.0, 0.6, 30.0);
    EXPECT_NEAR(num(up.lognormal), 0.5519, 5e-4);
    EXPECT_NEAR(num(up.twice_digital), 0.5222, 5e-4);
    EXPECT_GT(up.lognormal, up.twice_digital) << "a lognormal touch costs a little more than twice the digital";

    EXPECT_EQ(flat(100.0, 0.6, 30.0).lognormal, k_unit_e9);
    EXPECT_LT(num(flat(110.0, 0.0001, 30.0).lognormal), 1e-9);
    EXPECT_LT(num(flat(90.0, 0.0001, 30.0).lognormal), 1e-9);

    const auto down = flat(90.0, 0.6, 30.0);
    EXPECT_NEAR(num(down.lognormal), 0.5686334, 5e-6);
    EXPECT_NEAR(num(down.twice_digital), 0.5985397, 5e-6);
}

// continuous monitoring by a brownian-bridge crossing test per step, so the step count adds no bias
class TouchMonteCarlo : public ::testing::TestWithParam<double> {};

TEST_P(TouchMonteCarlo, LognormalFormulaAgrees) {
    const double vol = 0.6;
    const double years = 30.0 / 365.0;
    const double level = GetParam();
    const double side = level > 100.0 ? 1.0 : -1.0;
    const double barrier = side * std::log(level / 100.0);
    constexpr int k_paths = 20'000;
    constexpr int k_steps = 50;
    const double dt = years / k_steps;
    std::mt19937_64 rng{7};
    std::normal_distribution<double> normal;
    std::uniform_real_distribution<double> uniform;
    int touched = 0;
    for (int path = 0; path < k_paths; ++path) {
        double x = 0.0;
        for (int step = 0; step < k_steps; ++step) {
            // x is the log move towards the barrier, so a barrier below flips the drift
            const double next = x - side * 0.5 * vol * vol * dt + vol * std::sqrt(dt) * normal(rng);
            const double cross = next >= barrier ? 1.0
                                                 : std::exp(-2.0 * (barrier - x) * (barrier - next) / (vol * vol * dt));
            if (uniform(rng) < cross) {
                ++touched;
                break;
            }
            x = next;
        }
    }
    const double estimate = static_cast<double>(touched) / k_paths;
    const double formula = num(one_touch({.forward = e9(100.0),
                                          .barrier = e9(level),
                                          .years = e9(years),
                                          .smile = {.vol = e9(vol), .skew = e9(0.0)}})
                                   .lognormal);
    // four standard errors of a 20k-path estimate near one half
    EXPECT_NEAR(estimate, formula, 4.0 * std::sqrt(0.25 / k_paths));
}

INSTANTIATE_TEST_SUITE_P(UpAndDown, TouchMonteCarlo, ::testing::Values(110.0, 90.0));

} // namespace
} // namespace raikiri::options
