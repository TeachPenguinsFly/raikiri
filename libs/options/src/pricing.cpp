// built with -ffp-contract=off and the vendored normal cdf so mac and linux give the same doubles

#include <raikiri/options/pricing.hpp>

#include <lets_be_rational.h>
#include <normaldistribution.h>

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace raikiri::options {

namespace internal {

constexpr double k_e9_double = 1e9;
constexpr double k_days_per_year = 365.0;
constexpr double k_vol_points = 100.0;
constexpr double k_min_vol = 1e-4;
constexpr double k_max_vol = 10.0;

double to_double(FixedE9 value) noexcept {
    return static_cast<double>(value.raw) / k_e9_double;
}

FixedE9 to_fixed(double value) noexcept {
    const double scaled = value * k_e9_double;
    // the bound is the largest double below 2^63
    if (!std::isfinite(scaled) || std::fabs(scaled) >= 9.2233720368547748e18) {
        return FixedE9::undef();
    }
    return FixedE9::from_raw(std::llround(scaled));
}

bool positive(FixedE9 value) noexcept {
    return !value.is_undef() && value.raw > 0;
}

bool usable_vol(FixedE9 vol) noexcept {
    if (vol.is_undef()) {
        return false;
    }
    const double v = to_double(vol);
    return v >= k_min_vol && v <= k_max_vol;
}

bool usable(const Black76Input& in) noexcept {
    return positive(in.forward) && positive(in.strike) && positive(in.years) && positive(in.discount) &&
           in.discount.raw <= k_e9 && usable_vol(in.vol);
}

struct Terms {
    double forward{};
    double strike{};
    double vol{};
    double years{};
    double discount{};
    double theta_sign{}; // +1 call, -1 put, as the vendored code takes it
    double root_t{};
    double d1{};
    double d2{};
};

Terms terms_of(const Black76Input& in) noexcept {
    Terms t{.forward = to_double(in.forward),
            .strike = to_double(in.strike),
            .vol = to_double(in.vol),
            .years = to_double(in.years),
            .discount = to_double(in.discount),
            .theta_sign = in.put_call == PutCall::Call ? 1.0 : -1.0};
    t.root_t = std::sqrt(t.years);
    const double spread = t.vol * t.root_t;
    t.d1 = std::log(t.forward / t.strike) / spread + 0.5 * spread;
    t.d2 = t.d1 - spread;
    return t;
}

struct Greeks {
    double price{};
    double delta{};
    double gamma{};
    double vega{};
    double theta{};
};

Greeks greeks_of(const Terms& t) noexcept {
    const double density = norm_pdf(t.d1);
    Greeks g{};
    g.price = t.discount * Black(t.forward, t.strike, t.vol, t.years, t.theta_sign);
    g.delta = t.theta_sign * t.discount * norm_cdf(t.theta_sign * t.d1);
    g.gamma = t.discount * density / (t.forward * t.vol * t.root_t);
    g.vega = t.discount * t.forward * density * t.root_t;
    const double rate = -std::log(t.discount) / t.years;
    g.theta = -t.discount * t.forward * density * t.vol / (2.0 * t.root_t) + rate * g.price;
    return g;
}

Black76 to_fixed(const Greeks& g) noexcept {
    return Black76{.price = to_fixed(g.price),
                   .delta = to_fixed(g.delta),
                   .gamma = to_fixed(g.gamma),
                   .vega = to_fixed(g.vega / k_vol_points),
                   .theta = to_fixed(g.theta / k_days_per_year)};
}

double above(double d2, double root_t, double skew) noexcept {
    return norm_cdf(d2) - norm_pdf(d2) * root_t * skew;
}

} // namespace internal

Black76 black76(const Black76Input& in) noexcept {
    if (!internal::usable(in)) {
        return {};
    }
    return internal::to_fixed(internal::greeks_of(internal::terms_of(in)));
}

Black76 inverse(const Black76Input& in) noexcept {
    if (!internal::usable(in)) {
        return {};
    }
    const auto t = internal::terms_of(in);
    const auto usd = internal::greeks_of(t);
    internal::Greeks coin{};
    coin.price = usd.price / t.forward;
    coin.delta = usd.delta - coin.price;
    coin.gamma = t.forward * usd.gamma - 2.0 * coin.delta;
    coin.vega = usd.vega / t.forward;
    coin.theta = usd.theta / t.forward;
    return internal::to_fixed(coin);
}

FixedE9 implied_vol(const PremiumInput& in) noexcept {
    if (!internal::positive(in.price) || !internal::positive(in.forward) || !internal::positive(in.strike) ||
        !internal::positive(in.years) || !internal::positive(in.discount) || in.discount.raw > k_e9) {
        return FixedE9::undef();
    }
    const double forward = internal::to_double(in.forward);
    const double quoted = internal::to_double(in.price);
    const double usd = in.unit == OptionUnit::Base ? quoted * forward : quoted;
    const double sigma = ImpliedBlackVolatility(usd / internal::to_double(in.discount),
                                                forward,
                                                internal::to_double(in.strike),
                                                internal::to_double(in.years),
                                                in.put_call == PutCall::Call ? 1.0 : -1.0);
    if (!(sigma > 0.0) || sigma >= DBL_MAX) {
        return FixedE9::undef();
    }
    return internal::to_fixed(sigma);
}

std::optional<SmileAt> smile_at(std::span<const SmilePoint> smile, FixedE9 strike) noexcept {
    if (smile.size() < 2 || !internal::positive(strike)) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < smile.size(); ++i) {
        if (!internal::positive(smile[i].strike) || !internal::positive(smile[i].vol) ||
            (i > 0 && smile[i].strike <= smile[i - 1].strike)) {
            return std::nullopt;
        }
    }
    if (strike < smile.front().strike || strike > smile.back().strike) {
        return std::nullopt;
    }
    const auto log_k = [](FixedE9 k) { return std::log(internal::to_double(k)); };
    const auto vol = [&](std::size_t i) { return internal::to_double(smile[i].vol); };
    const auto slope = [&](std::size_t lo, std::size_t hi) {
        return (vol(hi) - vol(lo)) / (log_k(smile[hi].strike) - log_k(smile[lo].strike));
    };

    const auto upper = static_cast<std::size_t>(std::ranges::lower_bound(smile, strike, {}, &SmilePoint::strike) -
                                                smile.begin());
    if (smile[upper].strike == strike) {
        const std::size_t lo = upper == 0 ? 0 : upper - 1;
        const std::size_t hi = upper + 1 == smile.size() ? upper : upper + 1;
        return SmileAt{.vol = smile[upper].vol, .skew = internal::to_fixed(slope(lo, hi))};
    }
    const std::size_t lo = upper - 1;
    const double weight = (log_k(strike) - log_k(smile[lo].strike)) /
                          (log_k(smile[upper].strike) - log_k(smile[lo].strike));
    return SmileAt{.vol = internal::to_fixed(vol(lo) + weight * (vol(upper) - vol(lo))),
                   .skew = internal::to_fixed(slope(lo, upper))};
}

Digital digital(const DigitalInput& in) noexcept {
    if (!internal::positive(in.forward) || !internal::positive(in.strike) || !internal::positive(in.years) ||
        !internal::usable_vol(in.smile.vol) || in.smile.skew.is_undef()) {
        return {};
    }
    const auto t = internal::terms_of(
        Black76Input{.forward = in.forward, .strike = in.strike, .vol = in.smile.vol, .years = in.years});
    return Digital{.flat = internal::to_fixed(norm_cdf(t.d2)),
                   .skewed = internal::to_fixed(internal::above(t.d2, t.root_t, internal::to_double(in.smile.skew)))};
}

Touch one_touch(const TouchInput& in) noexcept {
    if (!internal::positive(in.forward) || !internal::positive(in.barrier) || !internal::positive(in.years) ||
        !internal::usable_vol(in.smile.vol) || in.smile.skew.is_undef()) {
        return {};
    }
    if (in.barrier == in.forward) {
        return Touch{.lognormal = k_unit_e9, .twice_digital = k_unit_e9};
    }
    const auto t = internal::terms_of(
        Black76Input{.forward = in.forward, .strike = in.barrier, .vol = in.smile.vol, .years = in.years});
    const double ratio = t.forward / t.strike;
    const bool up = in.barrier > in.forward;
    const double side = up ? 1.0 : -1.0;
    const double lognormal = norm_cdf(side * t.d2) + ratio * norm_cdf(side * t.d1);
    const double finish_above = internal::above(t.d2, t.root_t, internal::to_double(in.smile.skew));
    const double twice = 2.0 * (up ? finish_above : 1.0 - finish_above);
    return Touch{.lognormal = internal::to_fixed(std::min(lognormal, 1.0)),
                 .twice_digital = internal::to_fixed(std::clamp(twice, 0.0, 1.0))};
}

} // namespace raikiri::options
