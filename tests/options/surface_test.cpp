#include <raikiri/options/pricing.hpp>
#include <raikiri/options/surface.hpp>

#include <gtest/gtest.h>

#include <simdjson.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::options {
namespace {

FixedE9 e9(double value) {
    return FixedE9::from_raw(std::llround(value * 1e9));
}

double num(FixedE9 value) {
    return static_cast<double>(value.raw) / 1e9;
}

constexpr std::uint64_t k_ns_per_day = 86'400ULL * 1'000'000'000ULL;
constexpr double k_forward = 100.0;
const UnixNanos k_now{1'800'000'000ULL * 1'000'000'000ULL};

UnixNanos in_days(std::uint64_t days) {
    return UnixNanos{k_now.ns + days * k_ns_per_day};
}

double svi_total(const SviParams& p, double k) {
    const double x = k - num(p.m);
    return num(p.a) + num(p.b) * (num(p.rho) * x + std::sqrt(x * x + num(p.sigma) * num(p.sigma)));
}

double ssvi_total(double rho, double eta, double gamma, double theta, double k) {
    const double phi = eta / (std::pow(theta, gamma) * std::pow(1.0 + theta, 1.0 - gamma));
    const double u = phi * k + rho;
    return 0.5 * theta * (1.0 + rho * phi * k + std::sqrt(u * u + 1.0 - rho * rho));
}

// otm quotes a hair either side of the price the smile gives
SliceInput quotes_from(UnixNanos expiry, double forward, const std::function<double(double)>& total, double reach) {
    SliceInput slice{.expiry = expiry, .forward = e9(forward)};
    const auto years = years_between(k_now, expiry);
    for (double k = -reach; k <= reach + 1e-9; k += reach / 20.0) {
        const double strike = forward * std::exp(k);
        const auto pc = k >= 0.0 ? PutCall::Call : PutCall::Put;
        const auto price = black76({.forward = e9(forward),
                                    .strike = e9(strike),
                                    .vol = e9(std::sqrt(total(k) / num(years))),
                                    .years = years,
                                    .put_call = pc})
                               .price;
        slice.quotes.push_back({.venue = VenueId::Deribit,
                                .strike = e9(strike),
                                .put_call = pc,
                                .bid = e9(num(price) * 0.999),
                                .ask = e9(num(price) * 1.001),
                                .forward = e9(forward)});
    }
    return slice;
}

constexpr double k_rho = -0.3;
constexpr double k_eta = 1.2;
constexpr double k_gamma = 0.4;
constexpr std::array<std::uint64_t, 4> k_days{7, 30, 60, 120};
constexpr std::array<double, 4> k_thetas{0.004, 0.01, 0.02, 0.045};

std::vector<SliceInput> ssvi_chain(double level) {
    std::vector<SliceInput> chain;
    for (std::size_t i = 0; i < k_days.size(); ++i) {
        const double theta = k_thetas[i] * level;
        chain.push_back(quotes_from(
            in_days(k_days[i]),
            k_forward,
            [theta](double k) { return ssvi_total(k_rho, k_eta, k_gamma, theta, k); },
            4.0 * std::sqrt(theta)));
    }
    return chain;
}

TEST(SviFit, RecoversTheParametersBehindItsQuotes) {
    const SviParams truth{.a = e9(0.004), .b = e9(0.06), .rho = e9(-0.35), .m = e9(0.03), .sigma = e9(0.12)};
    const std::vector chain{quotes_from(in_days(30), k_forward, [&](double k) { return svi_total(truth, k); }, 0.5)};
    const auto surface = fit_surface(chain, k_now, {.min_delta = e9(0.0), .ssvi_tolerance = e9(0.0)});
    ASSERT_EQ(surface.slices.size(), 1U);
    const auto& fit = surface.slices[0];
    EXPECT_EQ(fit.quotes, 41U);
    EXPECT_NEAR(num(fit.svi.a), 0.004, 3e-4);
    EXPECT_NEAR(num(fit.svi.b), 0.06, 3e-3);
    EXPECT_NEAR(num(fit.svi.rho), -0.35, 0.03);
    EXPECT_NEAR(num(fit.svi.m), 0.03, 0.01);
    EXPECT_NEAR(num(fit.svi.sigma), 0.12, 0.01);
    EXPECT_LT(num(fit.svi_rmse), 1e-4);
    EXPECT_EQ(num(fit.inside), 1.0);
    const auto report = check_arbitrage(surface);
    EXPECT_EQ(report.butterfly, 0U);
    EXPECT_GT(num(report.min_density), 0.0);
}

TEST(SviFit, DropsCrossedOneSidedStaleAndWideQuotes) {
    const SviParams truth{.a = e9(0.004), .b = e9(0.06), .rho = e9(-0.35), .m = e9(0.03), .sigma = e9(0.12)};
    auto slice = quotes_from(in_days(30), k_forward, [&](double k) { return svi_total(truth, k); }, 0.5);
    slice.quotes[3].bid = e9(0.0);
    slice.quotes[5].ask = e9(num(slice.quotes[5].bid) * 0.5);
    slice.quotes[7].ts = UnixNanos{k_now.ns - 120 * 1'000'000'000ULL};
    slice.quotes[9].ask = e9(num(slice.quotes[9].ask) * 3.0);
    const std::vector chain{slice};
    const auto surface = fit_surface(chain, k_now, {.min_delta = e9(0.0)});
    ASSERT_EQ(surface.slices.size(), 1U);
    EXPECT_EQ(surface.slices[0].dropped, 4U);
    EXPECT_EQ(surface.slices[0].quotes, 37U);
}

TEST(SviFit, AVenueQuotingAVolPointHighShowsAsRich) {
    const SviParams truth{.a = e9(0.004), .b = e9(0.06), .rho = e9(-0.35), .m = e9(0.03), .sigma = e9(0.12)};
    const auto years = num(years_between(k_now, in_days(30)));
    auto slice = quotes_from(in_days(30), k_forward, [&](double k) { return svi_total(truth, k); }, 0.5);
    const auto rich = quotes_from(
        in_days(30),
        k_forward,
        [&](double k) {
            const double vol = std::sqrt(svi_total(truth, k) / years) + 0.01;
            return vol * vol * years;
        },
        0.5);
    for (auto quote : rich.quotes) {
        quote.venue = VenueId::Okx;
        slice.quotes.push_back(quote);
    }
    const std::vector chain{slice};
    const auto surface = fit_surface(chain, k_now, {.min_delta = e9(0.0)});
    ASSERT_EQ(surface.venues.size(), 2U);
    const auto& deribit = surface.venues[0].venue == VenueId::Deribit ? surface.venues[0] : surface.venues[1];
    const auto& okx = surface.venues[0].venue == VenueId::Okx ? surface.venues[0] : surface.venues[1];
    EXPECT_NEAR(num(okx.mean) - num(deribit.mean), 0.01, 1e-3);
    EXPECT_GT(num(okx.mean), 0.0);
    EXPECT_LT(num(deribit.mean), 0.0);
}

// vogt's slice, gatheral and jacquier's example 3.1: a smooth-looking smile with a negative density
TEST(Arbitrage, FlagsTheButterflyInVogtsSlice) {
    const SviParams vogt{.a = e9(-0.041), .b = e9(0.1331), .rho = e9(0.306), .m = e9(0.3586), .sigma = e9(0.4153)};
    EXPECT_LT(num(svi_density(vogt, e9(0.88))), -0.03);
    EXPECT_GT(num(svi_density(vogt, e9(-0.5))), 0.0);
    Surface surface{.model = SurfaceModel::Svi};
    surface.slices.push_back(
        {.years = e9(1.0), .forward = e9(k_forward), .svi = vogt, .k_low = e9(-1.0), .k_high = e9(1.0)});
    const auto report = check_arbitrage(surface);
    EXPECT_GT(report.butterfly, 0U);
    EXPECT_LT(num(report.min_density), -0.03);
}

Surface crossing_pair(bool repair) {
    Surface surface{.model = SurfaceModel::Svi, .calendar_repair = repair};
    surface.slices.push_back({.years = e9(0.1),
                              .forward = e9(k_forward),
                              .svi = {.a = e9(0.01), .b = e9(0.05), .rho = e9(0.0), .m = e9(0.0), .sigma = e9(0.1)},
                              .k_low = e9(-0.5),
                              .k_high = e9(0.5)});
    surface.slices.push_back({.years = e9(0.2),
                              .forward = e9(k_forward),
                              .svi = {.a = e9(0.014), .b = e9(0.02), .rho = e9(0.0), .m = e9(0.0), .sigma = e9(0.1)},
                              .k_low = e9(-0.5),
                              .k_high = e9(0.5)});
    return surface;
}

TEST(Arbitrage, FlagsACalendarCrossingAndTheRepairRemovesIt) {
    const auto crossed = check_arbitrage(crossing_pair(false));
    EXPECT_GT(crossed.calendar, 0U);
    EXPECT_EQ(crossed.butterfly, 0U);

    const auto repaired_surface = crossing_pair(true);
    const auto repaired = check_arbitrage(repaired_surface);
    EXPECT_EQ(repaired.calendar, 0U);
    // in the wing the later slice now carries the earlier one's total variance
    const auto wing = surface_at(repaired_surface, e9(k_forward * std::exp(0.45)), e9(0.2));
    ASSERT_TRUE(wing.has_value());
    const double w = num(wing->vol) * num(wing->vol) * 0.2;
    EXPECT_NEAR(w, 0.01 + 0.05 * std::sqrt(0.45 * 0.45 + 0.01), 1e-6);
}

TEST(Arbitrage, AFitThatFallsBackToSviRepairsItsCalendar) {
    std::vector<SliceInput> chain;
    const SviParams early{.a = e9(0.01), .b = e9(0.05), .rho = e9(0.0), .m = e9(0.0), .sigma = e9(0.1)};
    const SviParams late{.a = e9(0.014), .b = e9(0.02), .rho = e9(0.0), .m = e9(0.0), .sigma = e9(0.1)};
    chain.push_back(quotes_from(in_days(36), k_forward, [&](double k) { return svi_total(early, k); }, 0.5));
    chain.push_back(quotes_from(in_days(73), k_forward, [&](double k) { return svi_total(late, k); }, 0.5));
    const auto surface = fit_surface(chain, k_now, {.ssvi_tolerance = e9(0.0)});
    EXPECT_EQ(surface.model, SurfaceModel::Svi);
    EXPECT_TRUE(surface.calendar_repair);
    const auto report = check_arbitrage(surface);
    EXPECT_EQ(report.calendar, 0U);
    EXPECT_EQ(report.butterfly, 0U);
}

TEST(Ssvi, FitsASyntheticSurfaceInsideTheNoArbitrageConditions) {
    const auto chain = ssvi_chain(1.0);
    const auto surface = fit_surface(chain, k_now);
    ASSERT_EQ(surface.slices.size(), 4U);
    EXPECT_EQ(surface.model, SurfaceModel::Ssvi);
    EXPECT_NEAR(num(surface.ssvi.rho), k_rho, 0.03);
    EXPECT_NEAR(num(surface.ssvi.eta), k_eta, 0.06);
    EXPECT_NEAR(num(surface.ssvi.gamma), k_gamma, 0.05);
    for (std::size_t i = 0; i < k_thetas.size(); ++i) {
        EXPECT_NEAR(num(surface.slices[i].theta), k_thetas[i], k_thetas[i] * 0.01);
    }
    EXPECT_LT(num(surface.rmse), 5e-4);
    const auto report = check_arbitrage(surface);
    EXPECT_TRUE(report.ssvi_conditions);
    EXPECT_EQ(report.butterfly, 0U);
    EXPECT_EQ(report.calendar, 0U);
    EXPECT_GE(num(report.min_density), 0.0);
}

TEST(Ssvi, ConditionsRejectTooMuchCurvatureOrAFallingTheta) {
    const SsviParams steep{.rho = e9(0.5), .eta = e9(3.0), .gamma = e9(0.4)};
    const std::array one{e9(1.0)};
    EXPECT_FALSE(ssvi_arbitrage_free(steep, one));
    const SsviParams fine{.rho = e9(k_rho), .eta = e9(k_eta), .gamma = e9(k_gamma)};
    const std::array rising{e9(0.01), e9(0.02), e9(0.5)};
    const std::array falling{e9(0.02), e9(0.01)};
    EXPECT_TRUE(ssvi_arbitrage_free(fine, rising));
    EXPECT_FALSE(ssvi_arbitrage_free(fine, falling));
    EXPECT_GT(num(ssvi_density(fine, e9(0.02), e9(0.3))), 0.0);
}

TEST(Query, IsContinuousInStrikeAndExpiryAndItsSkewMatchesTheSlope) {
    const auto chain = ssvi_chain(1.0);
    const auto surface = fit_surface(chain, k_now);
    ASSERT_EQ(surface.model, SurfaceModel::Ssvi);
    const auto t1 = surface.slices[1].years;
    const auto t2 = surface.slices[2].years;
    for (double strike = 85.0; strike <= 115.0; strike += 0.5) {
        const auto years = e9(0.5 * (num(t1) + num(t2)));
        const auto here = surface_at(surface, e9(strike), years);
        const auto next = surface_at(surface, e9(strike + 0.01), years);
        ASSERT_TRUE(here.has_value() && next.has_value());
        EXPECT_FALSE(here->extrapolated);
        EXPECT_LT(std::fabs(num(next->vol) - num(here->vol)), 1e-4);
        const double slope = (num(next->vol) - num(here->vol)) / 0.01;
        EXPECT_NEAR(num(here->dvol_dstrike), slope, 2e-4);
        EXPECT_NEAR(num(here->skew), num(here->dvol_dstrike) * strike, 1e-6);
    }
    for (const double strike : {80.0, 95.0, 100.0, 110.0}) {
        const auto before = surface_at(surface, e9(strike), e9(num(t1) - 1e-6));
        const auto on = surface_at(surface, e9(strike), t1);
        const auto after = surface_at(surface, e9(strike), e9(num(t1) + 1e-6));
        ASSERT_TRUE(before.has_value() && on.has_value() && after.has_value());
        EXPECT_NEAR(num(before->vol), num(on->vol), 1e-5);
        EXPECT_NEAR(num(after->vol), num(on->vol), 1e-5);
    }
    const auto last = surface.slices.back().years;
    const auto at_last = surface_at(surface, e9(100.0), last);
    const auto beyond = surface_at(surface, e9(100.0), e9(2.0 * num(last)));
    ASSERT_TRUE(at_last.has_value() && beyond.has_value());
    EXPECT_FALSE(at_last->extrapolated);
    EXPECT_TRUE(beyond->extrapolated);
    EXPECT_NEAR(num(beyond->vol) * num(beyond->vol) * 2.0, num(at_last->vol) * num(at_last->vol), 1e-6);
    const auto far = surface_at(surface, e9(400.0), t1);
    ASSERT_TRUE(far.has_value());
    EXPECT_TRUE(far->extrapolated);
    EXPECT_EQ(far->skew.raw, 0);
    EXPECT_FALSE(surface_at(surface, e9(0.0), t1).has_value());
}

TEST(Proxy, RealizedVolScalesTheReferenceShape) {
    const auto chain = ssvi_chain(1.0);
    const auto reference = fit_surface(chain, k_now);
    const auto proxy = proxy_surface(
        reference, {.as_of = k_now, .spot = e9(150.0), .realized_vol = e9(0.9), .reference_realized_vol = e9(0.45)});
    EXPECT_EQ(proxy.source, SurfaceSource::Realized);
    EXPECT_EQ(proxy.model, SurfaceModel::Ssvi);
    EXPECT_NEAR(num(proxy.confidence), 0.25, 1e-9);
    ASSERT_EQ(proxy.slices.size(), reference.slices.size());
    for (std::size_t i = 0; i < proxy.slices.size(); ++i) {
        const auto years = reference.slices[i].years;
        const auto ref = surface_at(reference, reference.slices[i].forward, years);
        const auto coin = surface_at(proxy, e9(150.0), years);
        ASSERT_TRUE(ref.has_value() && coin.has_value());
        EXPECT_NEAR(num(coin->vol), 2.0 * num(ref->vol), 1e-6);
        EXPECT_LT(num(coin->skew), 0.0);
    }
    const auto report = check_arbitrage(proxy);
    EXPECT_TRUE(report.ssvi_conditions);
    EXPECT_EQ(report.butterfly, 0U);
    EXPECT_EQ(report.calendar, 0U);
}

TEST(Proxy, ListedOptionsSetTheLevelUnderTheBorrowedShape) {
    const auto reference = fit_surface(ssvi_chain(1.0), k_now);
    const SsviParams shape = reference.ssvi;
    std::vector<SliceInput> listed;
    const std::array<double, 2> thetas{0.03, 0.06};
    const std::array<std::uint64_t, 2> days{30, 60};
    for (std::size_t i = 0; i < 2; ++i) {
        const double theta = thetas[i];
        auto slice = quotes_from(
            in_days(days[i]),
            150.0,
            [&](double k) { return ssvi_total(num(shape.rho), num(shape.eta), num(shape.gamma), theta, k); },
            0.4);
        // a sparse book: every fourth strike
        std::vector<MarketQuote> sparse;
        for (std::size_t q = 0; q < slice.quotes.size(); q += 4) {
            sparse.push_back(slice.quotes[q]);
        }
        slice.quotes = sparse;
        listed.push_back(slice);
    }
    const auto proxy = proxy_surface(reference, {.as_of = k_now, .listed = listed});
    EXPECT_EQ(proxy.source, SurfaceSource::Borrowed);
    ASSERT_EQ(proxy.slices.size(), 2U);
    EXPECT_NEAR(num(proxy.slices[0].theta), 0.03, 0.03 * 0.01);
    EXPECT_NEAR(num(proxy.slices[1].theta), 0.06, 0.06 * 0.01);
    EXPECT_GT(num(proxy.confidence), 0.5);
    EXPECT_LE(num(proxy.confidence), 0.9);
}

TEST(Proxy, RealizedVolIsCloseToClose) {
    std::vector<FixedE9> closes{e9(100.0)};
    for (int i = 0; i < 30; ++i) {
        closes.push_back(e9(num(closes.back()) * std::exp(i % 2 == 0 ? 0.01 : -0.01)));
    }
    EXPECT_NEAR(num(realized_vol(closes, k_ns_per_day)), 0.01 * std::sqrt(365.0), 1e-6);
    EXPECT_TRUE(realized_vol(std::span(closes).first(1), k_ns_per_day).is_undef());
}

// the venues' btc chains on three expiries, recorded a few seconds apart

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

UnixNanos eight_utc(int y, unsigned m, unsigned d) {
    return UnixNanos{static_cast<std::uint64_t>(days_from_civil(y, m, d)) * k_ns_per_day + 8ULL * 3'600'000'000'000ULL};
}

// 16OCT26
UnixNanos deribit_date(std::string_view text) {
    static constexpr std::array<std::string_view, 12> months{
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    const auto day_digits = text.size() - 5;
    const auto day = static_cast<unsigned>(std::stoi(std::string{text.substr(0, day_digits)}));
    unsigned month = 0;
    for (unsigned i = 0; i < 12; ++i) {
        month = months[i] == text.substr(day_digits, 3) ? i + 1 : month;
    }
    return eight_utc(2000 + std::stoi(std::string{text.substr(day_digits + 3)}), month, day);
}

// 261016
UnixNanos okx_date(std::string_view text) {
    return eight_utc(2000 + std::stoi(std::string{text.substr(0, 2)}),
                     static_cast<unsigned>(std::stoi(std::string{text.substr(2, 2)})),
                     static_cast<unsigned>(std::stoi(std::string{text.substr(4, 2)})));
}

std::vector<std::string_view> split(std::string_view text) {
    std::vector<std::string_view> out;
    for (std::size_t at = 0; at <= text.size();) {
        const auto dash = std::min(text.find('-', at), text.size());
        out.push_back(text.substr(at, dash - at));
        at = dash + 1;
    }
    return out;
}

double decimal(std::string_view text) {
    return text.empty() ? 0.0 : std::stod(std::string{text});
}

std::string fixture(std::string_view name) {
    return std::string{RAIKIRI_SURFACE_FIXTURES} + "/" + std::string{name};
}

struct LiveChain {
    std::map<std::uint64_t, SliceInput> slices;
    UnixNanos now{};

    void add(UnixNanos expiry, MarketQuote quote) {
        auto& slice = slices[expiry.ns];
        slice.expiry = expiry;
        slice.quotes.push_back(quote);
        now = std::max(now, quote.ts);
    }
};

UnixNanos from_ms(std::uint64_t ms) {
    return UnixNanos{ms * 1'000'000ULL};
}

LiveChain live_chain() {
    LiveChain chain;
    simdjson::dom::parser parser;
    for (const auto row : parser.load(fixture("deribit_book_summary_btc.json"))["result"].get_array()) {
        const auto parts = split(std::string_view{row["instrument_name"].get_string()});
        double bid = 0.0;
        double ask = 0.0;
        if (row["bid_price"].get(bid) != simdjson::SUCCESS || row["ask_price"].get(ask) != simdjson::SUCCESS) {
            continue;
        }
        chain.add(deribit_date(parts[1]),
                  {.venue = VenueId::Deribit,
                   .strike = e9(decimal(parts[2])),
                   .put_call = parts[3] == "C" ? PutCall::Call : PutCall::Put,
                   .bid = e9(bid),
                   .ask = e9(ask),
                   .unit = OptionUnit::Base,
                   .forward = e9(row["underlying_price"].get_double()),
                   .ts = from_ms(static_cast<std::uint64_t>(row["creation_timestamp"].get_int64()))});
    }
    std::map<std::string, double> forwards;
    for (const auto row : parser.load(fixture("okx_opt_summary_btc.json"))["data"].get_array()) {
        forwards[std::string{std::string_view{row["instId"].get_string()}}] = decimal(
            std::string_view{row["fwdPx"].get_string()});
    }
    for (const auto row : parser.load(fixture("okx_tickers_btc.json"))["data"].get_array()) {
        const std::string id{std::string_view{row["instId"].get_string()}};
        const auto parts = split(id);
        chain.add(okx_date(parts[2]),
                  {.venue = VenueId::Okx,
                   .strike = e9(decimal(parts[3])),
                   .put_call = parts[4] == "C" ? PutCall::Call : PutCall::Put,
                   .bid = e9(decimal(std::string_view{row["bidPx"].get_string()})),
                   .ask = e9(decimal(std::string_view{row["askPx"].get_string()})),
                   .unit = OptionUnit::Base,
                   .forward = e9(forwards[id]),
                   .ts = from_ms(static_cast<std::uint64_t>(decimal(std::string_view{row["ts"].get_string()})))});
    }
    const auto bybit = parser.load(fixture("bybit_tickers_btc.json"));
    const auto bybit_ts = from_ms(static_cast<std::uint64_t>(bybit["time"].get_int64()));
    for (const auto row : bybit["result"]["list"].get_array()) {
        const auto parts = split(std::string_view{row["symbol"].get_string()});
        chain.add(deribit_date(parts[1]),
                  {.venue = VenueId::Bybit,
                   .strike = e9(decimal(parts[2])),
                   .put_call = parts[3] == "C" ? PutCall::Call : PutCall::Put,
                   .bid = e9(decimal(std::string_view{row["bid1Price"].get_string()})),
                   .ask = e9(decimal(std::string_view{row["ask1Price"].get_string()})),
                   .unit = OptionUnit::Quote,
                   .forward = e9(decimal(std::string_view{row["underlyingPrice"].get_string()})),
                   .ts = bybit_ts});
    }
    return chain;
}

TEST(LiveFixtures, DeribitOkxAndBybitBtcConvergeOnOneSurface) {
    const auto chain = live_chain();
    ASSERT_EQ(chain.slices.size(), 3U);
    std::vector<SliceInput> slices;
    for (const auto& [expiry, slice] : chain.slices) {
        slices.push_back(slice);
    }
    const auto surface = fit_surface(slices, chain.now);
    ASSERT_EQ(surface.slices.size(), 3U);
    ASSERT_NE(surface.model, SurfaceModel::None);
    std::printf("model %s rmse %.2f vp (svi %.2f, ssvi %.2f), ssvi rho %.3f eta %.3f gamma %.3f\n",
                surface.model == SurfaceModel::Ssvi ? "ssvi" : "svi",
                num(surface.rmse) * 100.0,
                num(surface.svi_rmse) * 100.0,
                num(surface.ssvi_rmse) * 100.0,
                num(surface.ssvi.rho),
                num(surface.ssvi.eta),
                num(surface.ssvi.gamma));
    for (const auto& slice : surface.slices) {
        std::printf("  %5.1f d  fwd %9.1f  n %3u dropped %3u  svi %.2f vp  ssvi %.2f vp  inside %.0f%%\n",
                    num(slice.years) * 365.0,
                    num(slice.forward),
                    slice.quotes,
                    slice.dropped,
                    num(slice.svi_rmse) * 100.0,
                    num(slice.ssvi_rmse) * 100.0,
                    num(slice.inside) * 100.0);
        EXPECT_LT(num(slice.svi_rmse), 0.01);
        EXPECT_GE(slice.quotes, 30U);
    }
    for (const auto& venue : surface.venues) {
        std::printf("  %-8s n %3u  mean %+.2f vp  rms %.2f vp\n",
                    std::string{to_string(venue.venue)}.c_str(),
                    venue.quotes,
                    num(venue.mean) * 100.0,
                    num(venue.rms) * 100.0);
    }
    EXPECT_EQ(surface.venues.size(), 3U);
    EXPECT_LT(num(surface.rmse), 0.01);
    const auto report = check_arbitrage(surface);
    std::printf("  butterfly %u calendar %u min g %.4f ssvi conditions %d\n",
                report.butterfly,
                report.calendar,
                num(report.min_density),
                report.ssvi_conditions ? 1 : 0);
    EXPECT_EQ(report.calendar, 0U);
    EXPECT_EQ(report.butterfly, 0U);
}

} // namespace
} // namespace raikiri::options
