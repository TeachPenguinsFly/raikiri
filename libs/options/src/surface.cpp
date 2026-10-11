// built with -ffp-contract=off like pricing.cpp, so a fit lands on the same doubles on mac and linux

#include <raikiri/options/pricing.hpp>
#include <raikiri/options/surface.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

namespace raikiri::options {

namespace {

constexpr double k_e9_double = 1e9;
constexpr double k_ns_per_year = 365.0 * 86'400.0 * 1e9;
constexpr double k_rho_bound = 0.999;
// lee's moment bound on the wing slope of w; the 4 often quoted is rogers and tehranchi's and leaves g < 0 far out
constexpr double k_wing_slope = 2.0;
constexpr double k_huge = 1e300;
constexpr double k_fitted_full = 0.05; // rmse at which a fitted surface's confidence reaches zero
constexpr double k_realized_confidence = 0.25;
constexpr int k_grid = 201;
constexpr int k_density_points = 61;
constexpr std::size_t k_constraints = 7;

double num(FixedE9 value) noexcept {
    return static_cast<double>(value.raw) / k_e9_double;
}

FixedE9 fixed(double value) noexcept {
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

struct Svi {
    double a{};
    double b{};
    double rho{};
    double m{};
    double sigma{};
};

struct Ssvi {
    double rho{};
    double eta{};
    double gamma{};
};

Svi svi_of(const SviParams& p) noexcept {
    return {.a = num(p.a), .b = num(p.b), .rho = num(p.rho), .m = num(p.m), .sigma = num(p.sigma)};
}

SviParams params_of(const Svi& p) noexcept {
    return {.a = fixed(p.a), .b = fixed(p.b), .rho = fixed(p.rho), .m = fixed(p.m), .sigma = fixed(p.sigma)};
}

Ssvi ssvi_of(const SsviParams& p) noexcept {
    return {.rho = num(p.rho), .eta = num(p.eta), .gamma = num(p.gamma)};
}

// w and its first two derivatives in k
struct Wk {
    double w{};
    double d1{};
    double d2{};
};

Wk svi_w(const Svi& p, double k) noexcept {
    const double x = k - p.m;
    const double root = std::sqrt(x * x + p.sigma * p.sigma);
    return {.w = p.a + p.b * (p.rho * x + root),
            .d1 = p.b * (p.rho + x / root),
            .d2 = p.b * p.sigma * p.sigma / (root * root * root)};
}

double phi_of(const Ssvi& p, double theta) noexcept {
    return p.eta / (std::pow(theta, p.gamma) * std::pow(1.0 + theta, 1.0 - p.gamma));
}

Wk ssvi_w(const Ssvi& p, double theta, double k) noexcept {
    const double phi = phi_of(p, theta);
    const double u = phi * k + p.rho;
    const double root = std::sqrt(u * u + 1.0 - p.rho * p.rho);
    const double half = 0.5 * theta;
    return {.w = half * (1.0 + p.rho * phi * k + root),
            .d1 = half * phi * (p.rho + u / root),
            .d2 = half * phi * phi * (1.0 - p.rho * p.rho) / (root * root * root)};
}

double density(const Wk& v, double k) noexcept {
    if (!(v.w > 0.0)) {
        return -1.0;
    }
    const double lead = 1.0 - k * v.d1 / (2.0 * v.w);
    return lead * lead - 0.25 * v.d1 * v.d1 * (1.0 / v.w + 0.25) + 0.5 * v.d2;
}

// the strikes a slice's density is checked over: the quoted range and half as much again either side
std::pair<double, double> density_range(double k_low, double k_high) noexcept {
    const double span = std::max(k_high - k_low, 0.1);
    return {k_low - 0.5 * span, k_high + 0.5 * span};
}

template <typename W> double least_density(W&& w_at, double k_low, double k_high, int points) noexcept {
    const auto [lo, hi] = density_range(k_low, k_high);
    double least = std::numeric_limits<double>::infinity();
    for (int g = 0; g < points; ++g) {
        const double k = lo + (hi - lo) * static_cast<double>(g) / (points - 1);
        least = std::min(least, density(w_at(k), k));
    }
    return least;
}

double vol_of(double w, double years) noexcept {
    return w > 0.0 ? std::sqrt(w / years) : 0.0;
}

double median_of(std::vector<double> values) noexcept {
    if (values.empty()) {
        return 0.0;
    }
    std::ranges::sort(values);
    const auto mid = values.size() / 2;
    return values.size() % 2 == 1 ? values[mid] : 0.5 * (values[mid - 1] + values[mid]);
}

// pool adjacent violators: the closest non-decreasing sequence in least squares
void non_decreasing(std::vector<double>& values) noexcept {
    std::vector<std::pair<double, std::size_t>> blocks;
    for (const double v : values) {
        blocks.emplace_back(v, 1);
        while (blocks.size() > 1 && blocks[blocks.size() - 2].first > blocks.back().first) {
            const auto top = blocks.back();
            blocks.pop_back();
            auto& below = blocks.back();
            const auto n = below.second + top.second;
            below.first = (below.first * static_cast<double>(below.second) +
                           top.first * static_cast<double>(top.second)) /
                          static_cast<double>(n);
            below.second = n;
        }
    }
    std::size_t at = 0;
    for (const auto& [v, n] : blocks) {
        for (std::size_t i = 0; i < n; ++i) {
            values[at++] = v;
        }
    }
}

template <std::size_t N, typename F>
std::array<double, N> nelder_mead(F&& f, std::array<double, N> start, std::array<double, N> step, int iterations) {
    std::array<std::array<double, N>, N + 1> x{};
    std::array<double, N + 1> fx{};
    for (std::size_t i = 0; i <= N; ++i) {
        x[i] = start;
        if (i > 0) {
            x[i][i - 1] += step[i - 1];
        }
        fx[i] = f(x[i]);
    }
    const auto toward = [](const std::array<double, N>& from, const std::array<double, N>& to, double t) {
        std::array<double, N> out{};
        for (std::size_t j = 0; j < N; ++j) {
            out[j] = from[j] + t * (to[j] - from[j]);
        }
        return out;
    };
    for (int it = 0; it < iterations; ++it) {
        std::array<std::size_t, N + 1> order{};
        for (std::size_t i = 0; i <= N; ++i) {
            order[i] = i;
        }
        std::ranges::sort(order, [&](std::size_t a, std::size_t b) { return fx[a] < fx[b]; });
        const auto best = order[0];
        const auto worst = order[N];
        const auto second = order[N - 1];
        if (std::fabs(fx[worst] - fx[best]) <= 1e-12 * (std::fabs(fx[best]) + 1e-300)) {
            break;
        }
        std::array<double, N> centre{};
        for (std::size_t i = 0; i <= N; ++i) {
            if (i != worst) {
                for (std::size_t j = 0; j < N; ++j) {
                    centre[j] += x[i][j] / static_cast<double>(N);
                }
            }
        }
        const auto reflected = toward(centre, x[worst], -1.0);
        const double fr = f(reflected);
        if (fr < fx[best]) {
            const auto expanded = toward(centre, x[worst], -2.0);
            const double fe = f(expanded);
            x[worst] = fe < fr ? expanded : reflected;
            fx[worst] = std::min(fe, fr);
        } else if (fr < fx[second]) {
            x[worst] = reflected;
            fx[worst] = fr;
        } else {
            const auto contracted = toward(centre, fr < fx[worst] ? reflected : x[worst], 0.5);
            const double fc = f(contracted);
            if (fc < std::min(fr, fx[worst])) {
                x[worst] = contracted;
                fx[worst] = fc;
            } else {
                for (std::size_t i = 0; i <= N; ++i) {
                    if (i != best) {
                        x[i] = toward(x[best], x[i], 0.5);
                        fx[i] = f(x[i]);
                    }
                }
            }
        }
    }
    std::size_t best = 0;
    for (std::size_t i = 1; i <= N; ++i) {
        if (fx[i] < fx[best]) {
            best = i;
        }
    }
    return x[best];
}

template <typename F> double golden_min(F&& f, double lo, double hi) {
    const double ratio = 0.5 * (std::sqrt(5.0) - 1.0);
    double c = hi - ratio * (hi - lo);
    double d = lo + ratio * (hi - lo);
    double fc = f(c);
    double fd = f(d);
    for (int i = 0; i < 80 && hi - lo > 1e-12 * (std::fabs(lo) + std::fabs(hi)); ++i) {
        if (fc < fd) {
            hi = d;
            d = c;
            fd = fc;
            c = hi - ratio * (hi - lo);
            fc = f(c);
        } else {
            lo = c;
            c = d;
            fc = fd;
            d = lo + ratio * (hi - lo);
            fd = f(d);
        }
    }
    return 0.5 * (lo + hi);
}

struct Point {
    double k{};
    double mid{};
    double bid{};
    double ask{};
    double weight{};
    double target{};
    VenueId venue{VenueId::Invalid};
};

struct Slice {
    UnixNanos expiry{};
    double years{};
    double forward{};
    std::vector<Point> points{};
    std::uint32_t dropped{0};
    Svi svi{};
    double theta{};
};

Slice prepare(const SliceInput& in, UnixNanos now, const SurfaceConfig& config) {
    Slice s{.expiry = in.expiry};
    if (in.expiry.ns <= now.ns || in.expiry.ns - now.ns < config.min_expiry_ns) {
        return s;
    }
    if (positive(in.forward)) {
        s.forward = num(in.forward);
    } else {
        std::vector<double> forwards;
        for (const auto& q : in.quotes) {
            if (positive(q.forward)) {
                forwards.push_back(num(q.forward));
            }
        }
        s.forward = median_of(std::move(forwards));
    }
    if (!(s.forward > 0.0)) {
        return s;
    }
    const auto years = years_between(now, in.expiry);
    s.years = num(years);
    const double root_t = std::sqrt(s.years);
    for (const auto& q : in.quotes) {
        if (!positive(q.strike)) {
            continue;
        }
        const double strike = num(q.strike);
        if ((q.put_call == PutCall::Call) != (strike >= s.forward)) {
            continue;
        }
        const bool stale = q.ts.ns != 0 && now.ns > q.ts.ns && now.ns - q.ts.ns > config.stale_ns;
        if (stale || !positive(q.forward) || !positive(q.bid) || !positive(q.ask) || q.ask < q.bid) {
            ++s.dropped;
            continue;
        }
        const PremiumInput base{
            .forward = q.forward, .strike = q.strike, .years = years, .put_call = q.put_call, .unit = q.unit};
        auto bid_in = base;
        bid_in.price = q.bid;
        auto ask_in = base;
        ask_in.price = q.ask;
        const auto bid_iv = implied_vol(bid_in);
        const auto ask_iv = implied_vol(ask_in);
        if (bid_iv.is_undef() || ask_iv.is_undef() || ask_iv.raw - bid_iv.raw > config.max_spread.raw) {
            ++s.dropped;
            continue;
        }
        Point p{.k = std::log(strike / s.forward), .bid = num(bid_iv), .ask = num(ask_iv), .venue = q.venue};
        p.mid = 0.5 * (p.bid + p.ask);
        p.target = p.mid;
        const double spread = p.mid * root_t;
        const double d1 = -p.k / spread + 0.5 * spread;
        // a far wing quoted at the venue's least tick has an iv that only says how big the tick is
        const double delta = 0.5 * std::erfc((q.put_call == PutCall::Call ? -d1 : d1) / std::numbers::sqrt2);
        if (delta < num(config.min_delta)) {
            ++s.dropped;
            continue;
        }
        if (config.weight == FitWeight::Vega) {
            p.weight = std::exp(-0.5 * d1 * d1);
        } else {
            p.weight = 1.0 / std::max(p.ask - p.bid, num(config.min_spread));
        }
        s.points.push_back(p);
    }
    std::ranges::sort(s.points, {}, &Point::k);
    return s;
}

template <std::size_t N> bool solve(std::array<std::array<double, N + 1>, N>& m, std::size_t n) noexcept {
    double scale = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            scale = std::max(scale, std::fabs(m[i][j]));
        }
    }
    if (!(scale > 0.0)) {
        return false;
    }
    for (std::size_t col = 0; col < n; ++col) {
        std::size_t pivot = col;
        for (std::size_t r = col + 1; r < n; ++r) {
            if (std::fabs(m[r][col]) > std::fabs(m[pivot][col])) {
                pivot = r;
            }
        }
        if (std::fabs(m[pivot][col]) <= 1e-13 * scale) {
            return false;
        }
        std::swap(m[col], m[pivot]);
        for (std::size_t r = 0; r < n; ++r) {
            if (r == col) {
                continue;
            }
            const double factor = m[r][col] / m[col][col];
            for (std::size_t c = col; c <= n; ++c) {
                m[r][c] -= factor * m[col][c];
            }
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        m[i][n] /= m[i][i];
    }
    return true;
}

struct Row {
    double k{};
    double w{};
    double omega{};
};

struct Inner {
    double a{};
    double d{};
    double c{};
    double cost{k_huge};
};

// gatheral's quasi-explicit step: for fixed m and σ, w = a + d·y + c·√(y² + 1) with y = (k − m)/σ is linear, so
// the bounded least squares is a small convex qp, solved exactly by trying every active set of at most three
Inner solve_inner(const std::vector<Row>& rows, double m, double s, double a_max) noexcept {
    std::array<std::array<double, 3>, 3> h{};
    std::array<double, 3> g{};
    double constant = 0.0;
    for (const auto& r : rows) {
        const double y = (r.k - m) / s;
        const std::array<double, 3> f{1.0, y, std::sqrt(y * y + 1.0)};
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t j = 0; j < 3; ++j) {
                h[i][j] += r.omega * f[i] * f[j];
            }
            g[i] += r.omega * f[i] * r.w;
        }
        constant += r.omega * r.w * r.w;
    }
    const double limit = k_wing_slope * s;
    // n · x >= bound, x = (a, d, c); a + c − |d| >= 0 keeps the least w, a + √(c² − d²), positive without a >= 0
    const std::array<std::array<double, 3>, k_constraints> normal{{
        {0.0, -1.0, 1.0},
        {0.0, 1.0, 1.0},
        {0.0, -1.0, -1.0},
        {0.0, 1.0, -1.0},
        {1.0, -1.0, 1.0},
        {1.0, 1.0, 1.0},
        {-1.0, 0.0, 0.0},
    }};
    const std::array<double, k_constraints> bound{0.0, 0.0, -limit, -limit, 0.0, 0.0, -a_max};
    Inner best{};
    for (unsigned mask = 0; mask < (1U << k_constraints); ++mask) {
        const auto active = static_cast<std::size_t>(std::popcount(mask));
        if (active > 3) {
            continue;
        }
        std::array<std::array<double, 7>, 6> kkt{};
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t j = 0; j < 3; ++j) {
                kkt[i][j] = 2.0 * h[i][j];
            }
            kkt[i][3 + active] = 2.0 * g[i];
        }
        std::size_t at = 3;
        for (std::size_t c = 0; c < k_constraints; ++c) {
            if ((mask & (1U << c)) == 0) {
                continue;
            }
            for (std::size_t j = 0; j < 3; ++j) {
                kkt[j][at] = -normal[c][j];
                kkt[at][j] = normal[c][j];
            }
            kkt[at][3 + active] = bound[c];
            ++at;
        }
        const std::size_t n = 3 + active;
        if (!solve<6>(kkt, n)) {
            continue;
        }
        const std::array<double, 3> x{kkt[0][n], kkt[1][n], kkt[2][n]};
        bool feasible = true;
        for (std::size_t c = 0; c < k_constraints && feasible; ++c) {
            const double lhs = normal[c][0] * x[0] + normal[c][1] * x[1] + normal[c][2] * x[2];
            feasible = lhs >= bound[c] - 1e-12 * (1.0 + std::fabs(bound[c]));
        }
        if (!feasible) {
            continue;
        }
        double cost = constant;
        for (std::size_t i = 0; i < 3; ++i) {
            cost -= 2.0 * g[i] * x[i];
            for (std::size_t j = 0; j < 3; ++j) {
                cost += x[i] * h[i][j] * x[j];
            }
        }
        if (cost < best.cost) {
            best = {.a = x[0], .d = x[1], .c = x[2], .cost = cost};
        }
    }
    return best;
}

Svi fit_svi(const Slice& s) {
    std::vector<Row> rows;
    rows.reserve(s.points.size());
    double a_max = 0.0;
    for (const auto& p : s.points) {
        const double w = p.target * p.target * s.years;
        // a vol residual is a w residual over 2σT
        const double scale = p.weight / (2.0 * p.target * s.years);
        rows.push_back({.k = p.k, .w = w, .omega = scale * scale});
        a_max = std::max(a_max, w);
    }
    const double lo = s.points.front().k;
    const double hi = s.points.back().k;
    const double span = std::max(hi - lo, 1e-3);
    const auto cost = [&](const std::array<double, 2>& x) {
        const double sigma = std::exp(x[1]);
        if (x[0] < lo - span || x[0] > hi + span || sigma < 1e-4 || sigma > 4.0) {
            return k_huge;
        }
        const auto inner = solve_inner(rows, x[0], sigma, a_max);
        if (inner.cost >= k_huge) {
            return k_huge;
        }
        const Svi p{.a = inner.a,
                    .b = inner.c / sigma,
                    .rho = inner.c > 0.0 ? inner.d / inner.c : 0.0,
                    .m = x[0],
                    .sigma = sigma};
        // a butterfly in the slice costs in proportion, so the search walks out of it rather than stopping at a wall
        const double least = least_density([&](double k) { return svi_w(p, k); }, lo, hi, k_density_points);
        return least < 0.0 ? inner.cost * (1.0 + 1e4 * -least) + 1e-300 : inner.cost;
    };
    std::vector<std::pair<double, std::array<double, 2>>> starts;
    for (int i = 0; i < 9; ++i) {
        for (const double fraction : {0.02, 0.05, 0.1, 0.2, 0.5, 1.0}) {
            const std::array<double, 2> x{lo + span * static_cast<double>(i) / 8.0, std::log(span * fraction)};
            starts.emplace_back(cost(x), x);
        }
    }
    std::ranges::sort(starts, {}, &std::pair<double, std::array<double, 2>>::first);
    std::array<double, 2> best = starts.front().second;
    double best_cost = starts.front().first;
    for (std::size_t i = 0; i < 3 && i < starts.size(); ++i) {
        const auto x = nelder_mead<2>(cost, starts[i].second, {span / 10.0, 0.5}, 400);
        if (const double c = cost(x); c < best_cost) {
            best = x;
            best_cost = c;
        }
    }
    const double sigma = std::exp(best[1]);
    const auto inner = solve_inner(rows, best[0], sigma, a_max);
    return {.a = inner.a,
            .b = inner.c / sigma,
            .rho = inner.c > 0.0 ? std::clamp(inner.d / inner.c, -1.0, 1.0) : 0.0,
            .m = best[0],
            .sigma = sigma};
}

// fits on mid, then again toward the nearest edge of each quote's band, so a fit inside bid/ask costs less
void fit_slice_svi(Slice& s) {
    for (auto& p : s.points) {
        p.target = p.mid;
    }
    s.svi = fit_svi(s);
    for (auto& p : s.points) {
        const double model = vol_of(svi_w(s.svi, p.k).w, s.years);
        p.target = 0.5 * (p.mid + std::clamp(model, p.bid, p.ask));
    }
    s.svi = fit_svi(s);
    for (auto& p : s.points) {
        p.target = p.mid;
    }
}

Ssvi ssvi_from(const std::array<double, 3>& x) noexcept {
    return {.rho = k_rho_bound * std::tanh(x[0]), .eta = std::exp(x[2]), .gamma = 1.0 / (1.0 + std::exp(-x[1]))};
}

// gatheral and jacquier's theorems 4.1 and 4.2 for every θ in [first, last]: under the power law the calendar
// bound always holds, θφ rises with θ and θφ² peaks at θ = 1 − 2γ, so the ends and that peak are enough
bool conditions_hold(const Ssvi& p, double first, double last) noexcept {
    if (!(std::fabs(p.rho) < 1.0) || !(p.eta > 0.0) || !(p.gamma >= 0.0 && p.gamma < 1.0) || !(first > 0.0) ||
        !(last >= first)) {
        return false;
    }
    const double lift = 1.0 + std::fabs(p.rho);
    const auto at = [&](double theta) {
        const double phi = phi_of(p, theta);
        return theta * phi * lift < 4.0 && theta * phi * phi * lift <= 4.0 + 1e-12;
    };
    const double peak = 1.0 - 2.0 * p.gamma;
    return at(first) && at(last) && (!(peak > first && peak < last) || at(peak));
}

double ssvi_cost(const Ssvi& p, const Slice& s, double theta) noexcept {
    double cost = 0.0;
    for (const auto& pt : s.points) {
        const double r = pt.weight * (vol_of(ssvi_w(p, theta, pt.k).w, s.years) - pt.target);
        cost += r * r;
    }
    return cost;
}

double fit_theta(const Ssvi& p, const Slice& s, double guess) {
    return golden_min([&](double theta) { return ssvi_cost(p, s, theta); }, 0.25 * guess, 4.0 * guess);
}

Ssvi fit_ssvi_shape(std::vector<Slice*>& slices) {
    const double first = slices.front()->theta;
    const double last = slices.back()->theta;
    const auto cost = [&](const std::array<double, 3>& x) {
        const auto p = ssvi_from(x);
        if (!conditions_hold(p, first, last)) {
            return k_huge;
        }
        double total = 0.0;
        for (const auto* s : slices) {
            total += ssvi_cost(p, *s, s->theta);
        }
        return std::isfinite(total) ? total : k_huge;
    };
    std::array<double, 3> best{};
    double best_cost = k_huge;
    for (const double rho : {-0.6, -0.2, 0.2}) {
        for (const double gamma : {-1.5, 0.0, 1.5}) {
            for (const double eta : {0.1, 0.3, 1.0, 2.0}) {
                const std::array<double, 3> x{std::atanh(rho / k_rho_bound), gamma, std::log(eta)};
                if (const double c = cost(x); c < best_cost) {
                    best = x;
                    best_cost = c;
                }
            }
        }
    }
    if (best_cost >= k_huge) {
        return {.rho = 0.0, .eta = 1e-3, .gamma = 0.25};
    }
    return ssvi_from(nelder_mead<3>(cost, best, {0.3, 0.5, 0.5}, 600));
}

Ssvi fit_ssvi(std::vector<Slice*>& slices) {
    std::vector<double> thetas;
    thetas.reserve(slices.size());
    for (const auto* s : slices) {
        thetas.push_back(std::max(svi_w(s->svi, 0.0).w, 1e-10));
    }
    non_decreasing(thetas);
    for (std::size_t i = 0; i < slices.size(); ++i) {
        slices[i]->theta = thetas[i];
    }
    for (int round = 0; round < 3; ++round) {
        const auto p = fit_ssvi_shape(slices);
        for (std::size_t i = 0; i < slices.size(); ++i) {
            thetas[i] = fit_theta(p, *slices[i], slices[i]->theta);
        }
        non_decreasing(thetas);
        for (std::size_t i = 0; i < slices.size(); ++i) {
            slices[i]->theta = thetas[i];
        }
    }
    // the shape goes last so the conditions hold over the θ it returns with
    return fit_ssvi_shape(slices);
}

// the chosen model on slice i, w held flat past the fitted strikes, with the calendar repair applied
struct SliceView {
    const Surface& surface;
    Ssvi ssvi{};
    double k_low{};
    double k_high{};

    explicit SliceView(const Surface& s) noexcept
        : surface{s}, ssvi{ssvi_of(s.ssvi)}, k_low{std::numeric_limits<double>::infinity()},
          k_high{-std::numeric_limits<double>::infinity()} {
        for (const auto& slice : s.slices) {
            k_low = std::min(k_low, num(slice.k_low));
            k_high = std::max(k_high, num(slice.k_high));
        }
    }

    [[nodiscard]] Wk raw(std::size_t i, double k) const noexcept {
        const auto& slice = surface.slices[i];
        return surface.model == SurfaceModel::Ssvi ? ssvi_w(ssvi, num(slice.theta), k) : svi_w(svi_of(slice.svi), k);
    }

    [[nodiscard]] std::pair<Wk, bool> ssvi_held(double theta, double k) const noexcept {
        const double clamped = std::clamp(k, k_low, k_high);
        auto v = ssvi_w(ssvi, theta, clamped);
        if (clamped != k) {
            v.d1 = 0.0;
            v.d2 = 0.0;
        }
        return {v, clamped != k};
    }

    [[nodiscard]] std::pair<Wk, bool> held(std::size_t i, double k) const noexcept {
        const auto& slice = surface.slices[i];
        // ssvi holds one strike range over the whole surface so the flat parts cannot cross in time
        const double lo = surface.model == SurfaceModel::Ssvi ? k_low : num(slice.k_low);
        const double hi = surface.model == SurfaceModel::Ssvi ? k_high : num(slice.k_high);
        const double clamped = std::clamp(k, lo, hi);
        auto v = raw(i, clamped);
        if (clamped != k) {
            v.d1 = 0.0;
            v.d2 = 0.0;
        }
        return {v, clamped != k};
    }

    // w only grows with the running max, and since a call grows with w the max of two butterfly-free slices'
    // calls is still convex in strike
    [[nodiscard]] std::pair<Wk, bool> at(std::size_t i, double k) const noexcept {
        auto out = held(i, k);
        if (surface.model == SurfaceModel::Svi && surface.calendar_repair) {
            for (std::size_t j = 0; j < i; ++j) {
                const auto earlier = held(j, k);
                if (earlier.first.w > out.first.w) {
                    out = earlier;
                }
            }
        }
        return out;
    }
};

std::uint32_t calendar_crossings(const SliceView& view) noexcept {
    std::uint32_t crossings = 0;
    const auto& slices = view.surface.slices;
    for (std::size_t i = 0; i + 1 < slices.size(); ++i) {
        const double lo = std::min(num(slices[i].k_low), num(slices[i + 1].k_low));
        const double hi = std::max(num(slices[i].k_high), num(slices[i + 1].k_high));
        for (int g = 0; g < k_grid; ++g) {
            const double k = lo + (hi - lo) * static_cast<double>(g) / (k_grid - 1);
            const double earlier = view.at(i, k).first.w;
            if (view.at(i + 1, k).first.w < earlier - 1e-12 * (1.0 + earlier)) {
                ++crossings;
            }
        }
    }
    return crossings;
}

struct Tally {
    VenueId venue{VenueId::Invalid};
    std::uint32_t n{0};
    double sum{};
    double squares{};
};

void score(Surface& out, const std::vector<Slice*>& slices) {
    const SliceView view{out};
    std::vector<Tally> tallies;
    double svi_sq = 0.0;
    double ssvi_sq = 0.0;
    double chosen_sq = 0.0;
    std::size_t total = 0;
    for (std::size_t i = 0; i < slices.size(); ++i) {
        const auto& s = *slices[i];
        auto& fit = out.slices[i];
        double svi = 0.0;
        double ssvi = 0.0;
        std::uint32_t inside = 0;
        for (const auto& p : s.points) {
            const double by_svi = vol_of(svi_w(s.svi, p.k).w, s.years);
            const double by_ssvi = vol_of(ssvi_w(view.ssvi, s.theta, p.k).w, s.years);
            const double chosen = vol_of(view.at(i, p.k).first.w, s.years);
            svi += (by_svi - p.mid) * (by_svi - p.mid);
            ssvi += (by_ssvi - p.mid) * (by_ssvi - p.mid);
            chosen_sq += (chosen - p.mid) * (chosen - p.mid);
            inside += chosen >= p.bid && chosen <= p.ask ? 1U : 0U;
            auto tally = std::ranges::find(tallies, p.venue, &Tally::venue);
            if (tally == tallies.end()) {
                tallies.push_back({.venue = p.venue});
                tally = tallies.end() - 1;
            }
            ++tally->n;
            tally->sum += p.mid - chosen;
            tally->squares += (p.mid - chosen) * (p.mid - chosen);
        }
        const auto n = static_cast<double>(s.points.size());
        fit.svi_rmse = fixed(std::sqrt(svi / n));
        fit.ssvi_rmse = fixed(std::sqrt(ssvi / n));
        fit.inside = fixed(static_cast<double>(inside) / n);
        svi_sq += svi;
        ssvi_sq += ssvi;
        total += s.points.size();
    }
    if (total == 0) {
        return;
    }
    const auto n = static_cast<double>(total);
    out.svi_rmse = fixed(std::sqrt(svi_sq / n));
    out.ssvi_rmse = fixed(std::sqrt(ssvi_sq / n));
    out.rmse = fixed(std::sqrt(chosen_sq / n));
    std::ranges::sort(tallies, {}, &Tally::venue);
    for (const auto& t : tallies) {
        const auto count = static_cast<double>(t.n);
        out.venues.push_back({.venue = t.venue,
                              .quotes = t.n,
                              .mean = fixed(t.sum / count),
                              .rms = fixed(std::sqrt(t.squares / count))});
    }
}

SliceFit slice_fit(const Slice& s) {
    return {.expiry = s.expiry,
            .years = fixed(s.years),
            .forward = fixed(s.forward),
            .theta = fixed(s.theta),
            .svi = params_of(s.svi),
            .k_low = fixed(s.points.front().k),
            .k_high = fixed(s.points.back().k),
            .quotes = static_cast<std::uint32_t>(s.points.size()),
            .dropped = s.dropped};
}

} // namespace

Surface fit_surface(std::span<const SliceInput> slices, UnixNanos now, const SurfaceConfig& config) {
    Surface out{.as_of = now};
    std::vector<Slice> prepared;
    for (const auto& in : slices) {
        auto s = prepare(in, now, config);
        if (s.years > 0.0 && s.points.size() >= std::max<std::size_t>(config.min_quotes, 3)) {
            prepared.push_back(std::move(s));
        }
    }
    std::ranges::sort(prepared, {}, &Slice::years);
    if (prepared.empty()) {
        return out;
    }
    std::vector<Slice*> fitted;
    for (auto& s : prepared) {
        fit_slice_svi(s);
        fitted.push_back(&s);
    }
    const auto ssvi = fit_ssvi(fitted);
    out.ssvi = {.rho = fixed(ssvi.rho), .eta = fixed(ssvi.eta), .gamma = fixed(ssvi.gamma)};
    for (const auto* s : fitted) {
        out.slices.push_back(slice_fit(*s));
    }
    out.source = SurfaceSource::Fitted;
    out.model = SurfaceModel::Ssvi;
    score(out, fitted);
    if (out.ssvi_rmse.raw > out.svi_rmse.raw + config.ssvi_tolerance.raw) {
        out.model = SurfaceModel::Svi;
        out.calendar_repair = calendar_crossings(SliceView{out}) > 0;
        out.venues.clear();
        score(out, fitted);
    }
    out.confidence = fixed(std::max(0.0, 1.0 - num(out.rmse) / k_fitted_full));
    return out;
}

FixedE9 forward_at(const Surface& surface, FixedE9 years) noexcept {
    const auto& s = surface.slices;
    if (s.empty() || !positive(years)) {
        return FixedE9::undef();
    }
    const auto upper = std::ranges::lower_bound(s, years, {}, &SliceFit::years);
    if (upper == s.begin()) {
        return s.front().forward;
    }
    if (upper == s.end()) {
        return s.back().forward;
    }
    const auto& lo = *(upper - 1);
    const double t = (num(years) - num(lo.years)) / (num(upper->years) - num(lo.years));
    return fixed(std::exp((1.0 - t) * std::log(num(lo.forward)) + t * std::log(num(upper->forward))));
}

std::optional<SurfacePoint> surface_at(const Surface& surface, FixedE9 strike, FixedE9 years) noexcept {
    if (surface.model == SurfaceModel::None || surface.slices.empty() || !positive(strike) || !positive(years)) {
        return std::nullopt;
    }
    const auto forward = forward_at(surface, years);
    if (!positive(forward)) {
        return std::nullopt;
    }
    const SliceView view{surface};
    const auto& s = surface.slices;
    const double t = num(years);
    const double k = std::log(num(strike) / num(forward));
    const auto upper = static_cast<std::size_t>(std::ranges::lower_bound(s, years, {}, &SliceFit::years) - s.begin());
    double w = 0.0;
    double d1 = 0.0;
    bool extrapolated = false;
    if (upper == 0 || upper == s.size()) {
        const auto i = upper == 0 ? 0 : s.size() - 1;
        const auto [v, held] = view.at(i, k);
        const double scale = upper == 0 ? t / num(s[0].years) : 1.0;
        w = v.w * scale;
        d1 = v.d1 * scale;
        extrapolated = held || years != s[i].years;
    } else if (surface.model == SurfaceModel::Ssvi) {
        // θ moves linearly in time and the slice in between is ssvi again, so the theorems still cover it
        const double a = (t - num(s[upper - 1].years)) / (num(s[upper].years) - num(s[upper - 1].years));
        const double theta = (1.0 - a) * num(s[upper - 1].theta) + a * num(s[upper].theta);
        const auto [v, held] = view.ssvi_held(theta, k);
        w = v.w;
        d1 = v.d1;
        extrapolated = held;
    } else {
        const auto [lo, lo_held] = view.at(upper - 1, k);
        const auto [hi, hi_held] = view.at(upper, k);
        const double a = (t - num(s[upper - 1].years)) / (num(s[upper].years) - num(s[upper - 1].years));
        w = (1.0 - a) * lo.w + a * hi.w;
        d1 = (1.0 - a) * lo.d1 + a * hi.d1;
        extrapolated = lo_held || hi_held;
    }
    if (!(w > 0.0)) {
        return std::nullopt;
    }
    const double vol = std::sqrt(w / t);
    const double skew = d1 / (2.0 * vol * t);
    return SurfacePoint{.vol = fixed(vol),
                        .skew = fixed(skew),
                        .dvol_dstrike = fixed(skew / num(strike)),
                        .extrapolated = extrapolated};
}

FixedE9 svi_density(const SviParams& svi, FixedE9 k) noexcept {
    const double x = num(k);
    return fixed(density(svi_w(svi_of(svi), x), x));
}

FixedE9 ssvi_density(const SsviParams& ssvi, FixedE9 theta, FixedE9 k) noexcept {
    if (!positive(theta)) {
        return FixedE9::undef();
    }
    const double x = num(k);
    return fixed(density(ssvi_w(ssvi_of(ssvi), num(theta), x), x));
}

bool ssvi_arbitrage_free(const SsviParams& ssvi, std::span<const FixedE9> thetas) noexcept {
    if (thetas.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < thetas.size(); ++i) {
        if (!positive(thetas[i]) || (i > 0 && thetas[i] < thetas[i - 1])) {
            return false;
        }
    }
    return conditions_hold(ssvi_of(ssvi), num(thetas.front()), num(thetas.back()));
}

ArbitrageReport check_arbitrage(const Surface& surface) noexcept {
    ArbitrageReport report{};
    if (surface.slices.empty() || surface.model == SurfaceModel::None) {
        return report;
    }
    const SliceView view{surface};
    double lowest = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < surface.slices.size(); ++i) {
        const auto& slice = surface.slices[i];
        const auto [lo, hi] = density_range(num(slice.k_low), num(slice.k_high));
        for (int g = 0; g < k_grid; ++g) {
            const double k = lo + (hi - lo) * static_cast<double>(g) / (k_grid - 1);
            const double value = density(view.raw(i, k), k);
            lowest = std::min(lowest, value);
            report.butterfly += value < -1e-6 ? 1U : 0U;
        }
    }
    report.min_density = fixed(lowest);
    report.calendar = calendar_crossings(view);
    std::vector<FixedE9> thetas;
    thetas.reserve(surface.slices.size());
    for (const auto& slice : surface.slices) {
        thetas.push_back(slice.theta);
    }
    report.ssvi_conditions = ssvi_arbitrage_free(surface.ssvi, thetas);
    return report;
}

Surface proxy_surface(const Surface& reference, const ProxyInput& in, const SurfaceConfig& config) {
    Surface out{.as_of = in.as_of, .ssvi = reference.ssvi};
    if (reference.slices.empty() || !positive(reference.ssvi.eta)) {
        return out;
    }
    const auto shape = ssvi_of(reference.ssvi);
    std::vector<Slice> listed;
    for (const auto& slice : in.listed) {
        auto s = prepare(slice, in.as_of, config);
        if (s.years > 0.0 && s.points.size() >= 3) {
            listed.push_back(std::move(s));
        }
    }
    std::ranges::sort(listed, {}, &Slice::years);
    out.model = SurfaceModel::Ssvi;
    if (!listed.empty()) {
        std::vector<double> thetas;
        std::size_t quotes = 0;
        for (const auto& s : listed) {
            const auto atm = std::ranges::min_element(s.points, {}, [](const Point& p) { return std::fabs(p.k); });
            thetas.push_back(fit_theta(shape, s, atm->mid * atm->mid * s.years));
            quotes += s.points.size();
        }
        non_decreasing(thetas);
        std::vector<Slice*> slices;
        for (std::size_t i = 0; i < listed.size(); ++i) {
            listed[i].theta = thetas[i];
            slices.push_back(&listed[i]);
            out.slices.push_back(slice_fit(listed[i]));
        }
        out.source = SurfaceSource::Borrowed;
        score(out, slices);
        out.svi_rmse = FixedE9::undef();
        for (auto& fit : out.slices) {
            fit.svi_rmse = FixedE9::undef();
        }
        const double depth = std::min(1.0, static_cast<double>(quotes) / 30.0);
        out.confidence = fixed((0.5 + 0.4 * depth) * std::max(0.0, 1.0 - num(out.rmse) / k_fitted_full));
        return out;
    }
    if (!positive(in.spot) || !positive(in.realized_vol) || !positive(in.reference_realized_vol)) {
        out.model = SurfaceModel::None;
        return out;
    }
    const double ratio = num(in.realized_vol) / num(in.reference_realized_vol);
    for (const auto& ref : reference.slices) {
        if (ref.expiry.ns <= in.as_of.ns) {
            continue;
        }
        auto slice = ref;
        slice.years = years_between(in.as_of, ref.expiry);
        slice.forward = in.spot;
        slice.theta = fixed(num(ref.theta) * ratio * ratio * num(slice.years) / num(ref.years));
        // the same standardised moneyness k/√θ
        slice.k_low = fixed(num(ref.k_low) * ratio);
        slice.k_high = fixed(num(ref.k_high) * ratio);
        slice.svi = {};
        slice.quotes = 0;
        slice.dropped = 0;
        slice.svi_rmse = {};
        slice.ssvi_rmse = {};
        slice.inside = {};
        out.slices.push_back(slice);
    }
    std::vector<double> thetas;
    thetas.reserve(out.slices.size());
    for (const auto& slice : out.slices) {
        thetas.push_back(num(slice.theta));
    }
    non_decreasing(thetas);
    for (std::size_t i = 0; i < thetas.size(); ++i) {
        out.slices[i].theta = fixed(thetas[i]);
    }
    out.source = SurfaceSource::Realized;
    out.confidence = fixed(out.slices.empty() ? 0.0 : k_realized_confidence);
    if (out.slices.empty()) {
        out.model = SurfaceModel::None;
    }
    return out;
}

FixedE9 realized_vol(std::span<const FixedE9> closes, std::uint64_t step_ns) noexcept {
    if (closes.size() < 2 || step_ns == 0) {
        return FixedE9::undef();
    }
    double squares = 0.0;
    for (std::size_t i = 1; i < closes.size(); ++i) {
        if (!positive(closes[i]) || !positive(closes[i - 1])) {
            return FixedE9::undef();
        }
        const double r = std::log(num(closes[i]) / num(closes[i - 1]));
        squares += r * r;
    }
    const double per_step = squares / static_cast<double>(closes.size() - 1);
    return fixed(std::sqrt(per_step * k_ns_per_year / static_cast<double>(step_ns)));
}

} // namespace raikiri::options
