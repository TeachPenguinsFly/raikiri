#include <raikiri/optiondata/board.hpp>
#include <raikiri/optiondata/queries.hpp>
#include <raikiri/optiondata/routes.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace raikiri;
using namespace raikiri::optiondata;

namespace {

// 2026-11-02 08:00:00 utc
constexpr UnixNanos k_now{1'793'606'400'000'000'000ULL};
constexpr std::uint64_t k_day_ns = 86'400ULL * 1'000'000'000ULL;
constexpr double k_spot = 100'000.0;

FixedE9 e9(double value) {
    return FixedE9::from_raw(std::llround(value * 1e9));
}

double as_double(FixedE9 value) {
    return static_cast<double>(value.raw) / 1e9;
}

double normal_cdf(double x) {
    return 0.5 * std::erfc(-x / std::sqrt(2.0));
}

double forward_for(int days) {
    return k_spot * (1.0 + 0.0002 * days);
}

// the smile every venue quotes around, with a venue's own bias on top
double true_vol(double strike, double forward, double years) {
    const double k = std::log(strike / forward);
    return 0.5 + 0.05 * std::sqrt(years) - 0.15 * k + 0.4 * k * k;
}

double black(double forward, double strike, double vol, double years, PutCall pc) {
    const double sd = vol * std::sqrt(years);
    const double d1 = (std::log(forward / strike) + 0.5 * sd * sd) / sd;
    const double d2 = d1 - sd;
    return pc == PutCall::Call ? forward * normal_cdf(d1) - strike * normal_cdf(d2)
                               : strike * normal_cdf(-d2) - forward * normal_cdf(-d1);
}

struct VenueSpec {
    VenueId venue{};
    OptionUnit premium{};
    bool greeks{false};
    double bias{0.0}; // in vol
    int strike_step{5'000};
};

const std::vector<VenueSpec> k_venues{
    {.venue = VenueId::Deribit, .premium = OptionUnit::Base, .greeks = true, .bias = 0.0},
    {.venue = VenueId::Derive, .premium = OptionUnit::Quote, .greeks = true, .bias = 0.004},
    {.venue = VenueId::Okx, .premium = OptionUnit::Base, .greeks = false, .bias = -0.01, .strike_step = 10'000},
};

const std::vector<int> k_days{7, 30, 90};

std::vector<OptionQuote> fixture() {
    std::vector<OptionQuote> out;
    std::uint32_t symbol = 1;
    for (const auto& spec : k_venues) {
        for (const int days : k_days) {
            const UnixNanos expiry{k_now.ns + static_cast<std::uint64_t>(days) * k_day_ns};
            const double years = days / 365.0;
            const double forward = forward_for(days);
            for (int strike = 70'000; strike <= 140'000; strike += spec.strike_step) {
                for (const auto pc : {PutCall::Call, PutCall::Put}) {
                    const double vol = true_vol(strike, forward, years) + spec.bias;
                    const double usd = black(forward, strike, vol, years, pc);
                    const double half = std::max(usd * 0.01, 2.0);
                    // a coin premium is the usd price over the forward
                    const double scale = spec.premium == OptionUnit::Base ? forward : 1.0;
                    OptionQuote q{.instrument = InstrumentId::make(spec.venue, MarketType::Option, SymbolId{symbol++}),
                                  .key = {.underlying = *UnderlyingText::from("BTC"),
                                          .expiry = expiry,
                                          .strike = e9(strike),
                                          .put_call = pc},
                                  .symbol = *SymbolText::from("BTC-OPT"),
                                  .premium = spec.premium,
                                  .greeks_unit = spec.premium,
                                  .bid = e9((usd - half) / scale),
                                  .ask = e9((usd + half) / scale),
                                  .bid_size = e9(1.5),
                                  .ask_size = e9(2.0),
                                  .open_interest = e9(120.0),
                                  .volume_24h = e9(15.0),
                                  .ts = k_now};
                    if (usd - half <= 0.0) {
                        q.bid = FixedE9::undef();
                    }
                    if (spec.greeks) {
                        OptionGreeks g{};
                        g.instrument = q.instrument;
                        const double sd = vol * std::sqrt(years);
                        const double d1 = (std::log(forward / strike) + 0.5 * sd * sd) / sd;
                        const double pdf = std::exp(-0.5 * d1 * d1) / std::sqrt(2.0 * 3.141592653589793);
                        g.delta = e9(pc == PutCall::Call ? normal_cdf(d1) : normal_cdf(d1) - 1.0);
                        g.gamma = e9(pdf / (forward * sd));
                        g.vega = e9(forward * pdf * std::sqrt(years) / 100.0);
                        g.theta = e9(-forward * pdf * vol / (2.0 * std::sqrt(years)) / 365.0);
                        g.rho = FixedE9::undef();
                        g.mark_iv = e9(vol);
                        g.bid_iv = FixedE9::undef();
                        g.ask_iv = FixedE9::undef();
                        g.underlying_price = e9(forward);
                        g.mark_price = e9(usd / scale);
                        g.index_price = e9(k_spot);
                        q.greeks = g;
                    }
                    out.push_back(q);
                }
            }
        }
    }
    return out;
}

class FakeMarket final : public OptionMarket {
public:
    explicit FakeMarket(std::vector<OptionQuote> quotes) : quotes_{std::move(quotes)} {}

    [[nodiscard]] std::vector<UnderlyingListing> underlyings() const override {
        std::vector<ListedOption> listed;
        for (const auto& q : quotes_) {
            listed.push_back({.venue = q.instrument.venue(), .key = q.key});
        }
        return summarise(listed);
    }

    void
    read(std::string_view underlying, std::optional<UnixNanos> expiry, std::vector<OptionQuote>& out) const override {
        ++reads;
        out.clear();
        for (const auto& q : quotes_) {
            if (q.key.underlying.view() == underlying && (!expiry.has_value() || q.key.expiry == *expiry)) {
                out.push_back(q);
            }
        }
    }

    mutable int reads{0};

private:
    std::vector<OptionQuote> quotes_;
};

std::vector<OptionQuote> of_expiry(const std::vector<OptionQuote>& all, int days) {
    std::vector<OptionQuote> out;
    for (const auto& q : all) {
        if (q.key.expiry.ns == k_now.ns + static_cast<std::uint64_t>(days) * k_day_ns) {
            out.push_back(q);
        }
    }
    return out;
}

const QuoteView* venue_in(const SideView& side, VenueId venue) {
    for (const auto& v : side.venues) {
        if (v.venue == venue) {
            return &v;
        }
    }
    return nullptr;
}

} // namespace

TEST(OptionData, SummariseCountsVenuesExpiriesAndStrikes) {
    const auto quotes = fixture();
    std::vector<ListedOption> listed;
    for (const auto& q : quotes) {
        listed.push_back({.venue = q.instrument.venue(), .key = q.key});
    }
    const auto all = summarise(listed);
    ASSERT_EQ(all.size(), 1U);
    EXPECT_EQ(all[0].underlying.view(), "BTC");
    ASSERT_EQ(all[0].venues.size(), 3U);
    ASSERT_EQ(all[0].expiries.size(), 3U);
    EXPECT_LT(all[0].expiries[0].expiry, all[0].expiries[1].expiry);
    EXPECT_EQ(all[0].expiries[0].strikes, 15U);
    EXPECT_EQ(all[0].expiries[0].venues.size(), 3U);
}

TEST(OptionData, ForwardComesFromParityAcrossVenues) {
    const auto quotes = of_expiry(fixture(), 30);
    const auto forward = expiry_forward(quotes);
    EXPECT_EQ(forward.from, ForwardFrom::Parity);
    EXPECT_NEAR(as_double(forward.value), forward_for(30), forward_for(30) * 0.002);
}

TEST(OptionData, ForwardFallsBackToTheVenuesUnderlying) {
    auto quotes = of_expiry(fixture(), 30);
    for (auto& q : quotes) {
        q.ask = FixedE9::undef();
    }
    const auto forward = expiry_forward(quotes);
    EXPECT_EQ(forward.from, ForwardFrom::Venue);
    EXPECT_NEAR(as_double(forward.value), forward_for(30), 1e-6);
}

TEST(OptionData, ChainConvertsCoinPremiumsAtTheIndexAndFlagsModelGreeks) {
    const auto quotes = of_expiry(fixture(), 30);
    const UnixNanos expiry{k_now.ns + 30 * k_day_ns};
    const auto chain = build_chain(quotes, expiry, k_now);
    ASSERT_EQ(chain.rows.size(), 15U);
    EXPECT_NEAR(as_double(chain.years), 30.0 / 365.0, 1e-9);
    EXPECT_EQ(as_double(chain.atm_strike), 100'000.0);

    const auto& row = chain.rows[6]; // 100000
    ASSERT_EQ(as_double(row.strike), 100'000.0);
    const auto* deribit = venue_in(row.call, VenueId::Deribit);
    const auto* derive = venue_in(row.call, VenueId::Derive);
    const auto* okx = venue_in(row.call, VenueId::Okx);
    ASSERT_NE(deribit, nullptr);
    ASSERT_NE(derive, nullptr);
    ASSERT_NE(okx, nullptr);

    EXPECT_EQ(deribit->premium, OptionUnit::Base);
    EXPECT_NEAR(as_double(deribit->bid_usd), as_double(deribit->bid) * k_spot, 1e-3);
    EXPECT_EQ(deribit->iv, Figure::Venue);
    EXPECT_EQ(deribit->greeks, Figure::Venue);
    EXPECT_NEAR(as_double(deribit->delta), 0.53, 0.02);

    EXPECT_EQ(derive->premium, OptionUnit::Quote);
    EXPECT_EQ(derive->bid_usd, derive->bid);
    // derive publishes no bid iv, so it is implied
    EXPECT_NEAR(as_double(derive->bid_iv), as_double(derive->mark_iv), 0.02);

    EXPECT_EQ(okx->iv, Figure::Model);
    EXPECT_EQ(okx->greeks, Figure::Model);
    EXPECT_EQ(okx->greeks_unit, OptionUnit::Quote);
    EXPECT_NEAR(as_double(okx->mark_iv), true_vol(100'000, forward_for(30), 30.0 / 365.0) - 0.01, 0.003);
    EXPECT_GT(as_double(okx->delta), 0.45);
    EXPECT_LT(as_double(okx->delta), 0.6);
    EXPECT_GT(as_double(okx->vega), 0.0);
    EXPECT_LT(as_double(okx->theta), 0.0);
    EXPECT_EQ(as_double(okx->open_interest), 120.0);

    // derive's +0.4 vol is the richest bid, okx's -1 the cheapest ask
    EXPECT_EQ(row.call.bid.venue, VenueId::Derive);
    EXPECT_EQ(row.call.ask.venue, VenueId::Okx);
    EXPECT_EQ(row.call.bid.usd, derive->bid_usd);
}

TEST(OptionData, ChainVenueFilterKeepsTheForward) {
    const auto quotes = of_expiry(fixture(), 30);
    const UnixNanos expiry{k_now.ns + 30 * k_day_ns};
    const auto all = build_chain(quotes, expiry, k_now);
    const std::vector<VenueId> only{VenueId::Okx};
    const auto okx = build_chain(quotes, expiry, k_now, only);
    EXPECT_EQ(okx.forward.value, all.forward.value);
    ASSERT_EQ(okx.rows.size(), 8U);
    ASSERT_EQ(okx.venues.size(), 1U);
    for (const auto& row : okx.rows) {
        ASSERT_EQ(row.call.venues.size(), 1U);
        EXPECT_EQ(row.call.venues[0].venue, VenueId::Okx);
    }
}

TEST(OptionData, SurfaceFindsAtmSkewAndWings) {
    const auto view = build_surface(fixture(), k_now);
    ASSERT_NE(view.model, options::SurfaceModel::None);
    ASSERT_EQ(view.expiries.size(), 3U);
    for (const auto& e : view.expiries) {
        ASSERT_TRUE(e.fit.has_value());
        const double years = as_double(e.years);
        const double forward = as_double(e.forward.value);
        EXPECT_NEAR(as_double(e.atm_iv), true_vol(forward, forward, years), 0.01);
        EXPECT_GT(as_double(e.call25_strike), forward);
        EXPECT_LT(as_double(e.put25_strike), forward);
        EXPECT_LT(as_double(e.rr25), 0.0);
        EXPECT_GT(as_double(e.bf25), 0.0);
        EXPECT_EQ(e.fitted.size(), k_fitted_points);
        EXPECT_EQ(e.residuals.size(), 3U);
        for (const auto& point : e.smile) {
            EXPECT_EQ(point.put_call == PutCall::Call, point.strike >= e.forward.value);
        }
    }
    // atm term structure rises with √T in the fixture
    EXPECT_LT(view.expiries[0].atm_iv, view.expiries[2].atm_iv);
    for (const auto& r : view.expiries[1].residuals) {
        if (r.venue == VenueId::Derive) {
            EXPECT_GT(as_double(r.mean), 0.0);
        }
        if (r.venue == VenueId::Okx) {
            EXPECT_LT(as_double(r.mean), 0.0);
        }
    }
}

TEST(OptionData, ExpiryResolvesByDateOrInstant) {
    const FakeMarket market{fixture()};
    const auto all = market.underlyings();
    ASSERT_EQ(all.size(), 1U);
    const auto first = all[0].expiries[0].expiry;
    EXPECT_EQ(resolve_expiry(all[0], "20261109"), first);
    EXPECT_EQ(resolve_expiry(all[0], std::to_string(first.ns)), first);
    EXPECT_FALSE(resolve_expiry(all[0], "20261110").has_value());
    EXPECT_FALSE(resolve_expiry(all[0], "12x").has_value());
}

TEST(OptionData, SurfaceIsRefittedOnlyAfterTheRefresh) {
    const FakeMarket market{fixture()};
    UnixNanos now = k_now;
    GetSurface surface{market, [&now] { return now; }, Refit::Inline, 3'000'000'000ULL};
    ASSERT_TRUE(surface("BTC", {}).has_value());
    ASSERT_TRUE(surface("BTC", {}).has_value());
    EXPECT_EQ(market.reads, 1);
    now = now + 3'000'000'000ULL;
    ASSERT_TRUE(surface("BTC", {}).has_value());
    EXPECT_EQ(market.reads, 2);
    const std::vector<VenueId> one{VenueId::Deribit};
    ASSERT_TRUE(surface("BTC", one).has_value());
    EXPECT_EQ(market.reads, 3);
    EXPECT_FALSE(surface("ETH", {}).has_value());
}

TEST(OptionData, BackgroundRefitAnswersWithTheLastFit) {
    const FakeMarket market{fixture()};
    UnixNanos now = k_now;
    GetSurface surface{market, [&now] { return now; }, Refit::Background, 3'000'000'000ULL};
    const auto first = surface("BTC", {});
    ASSERT_TRUE(first.has_value());
    now = now + 3'000'000'000ULL;
    const auto stale = surface("BTC", {});
    ASSERT_TRUE(stale.has_value());
    EXPECT_EQ(stale->as_of, k_now);
    EXPECT_EQ(market.reads, 2);
    surface.settle();
    const auto next = surface("BTC", {});
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->as_of, now);
    EXPECT_EQ(market.reads, 2);
}

TEST(OptionData, RoutesAnswerAndRefuse) {
    const FakeMarket market{fixture()};
    const auto clock = [] { return k_now; };
    const ListUnderlyings underlyings{market};
    const GetChain chain{market, clock};
    GetSurface surface{market, clock, Refit::Inline};
    const OptionRoutes routes{underlyings, chain, surface};
    const auto get = [&routes](std::string_view path, std::string_view query) {
        return routes({.method = "GET", .path = path, .query = query});
    };

    EXPECT_FALSE(get("/v1/markets", "").has_value());
    EXPECT_EQ(routes({.method = "POST", .path = "/v1/options/chain"})->status, 405);
    EXPECT_EQ(get("/v1/options/chain", "underlying=BTC")->status, 400);
    EXPECT_EQ(get("/v1/options/chain", "underlying=ETH&expiry=20261109")->status, 404);
    EXPECT_EQ(get("/v1/options/chain", "underlying=BTC&expiry=20261110")->status, 404);
    EXPECT_EQ(get("/v1/options/chain", "underlying=BTC&expiry=20261109&venue=nowhere")->status, 400);
    EXPECT_EQ(get("/v1/options/surface", "")->status, 400);

    const auto list = get("/v1/options/underlyings", "");
    ASSERT_EQ(list->status, 200);
    EXPECT_NE(list->body.find(R"("date":"20261202")"), std::string::npos);

    const auto rows = get("/v1/options/chain", "underlying=BTC&expiry=20261202&venue=deribit,okx");
    ASSERT_EQ(rows->status, 200);
    EXPECT_NE(rows->body.find(R"("forward_source":"parity")"), std::string::npos);
    EXPECT_NE(rows->body.find(R"("premium_unit":"base")"), std::string::npos);
    EXPECT_NE(rows->body.find(R"("greeks_source":"model")"), std::string::npos);
    EXPECT_EQ(rows->body.find(R"("venue":"derive")"), std::string::npos);

    const auto fit = get("/v1/options/surface", "underlying=BTC");
    ASSERT_EQ(fit->status, 200);
    EXPECT_NE(fit->body.find(R"("rr25":"-)"), std::string::npos);

    // RAIKIRI_OPTIONS_FIXTURES=<dir> writes these answers out for the terminal's mock run
    if (const char* dir = std::getenv("RAIKIRI_OPTIONS_FIXTURES"); dir != nullptr) {
        const auto write = [dir](const char* name, const std::string& body) {
            std::ofstream{std::string{dir} + "/" + name} << body;
        };
        write("underlyings.json", list->body);
        for (const auto* date : {"20261109", "20261202", "20270131"}) {
            write((std::string{"chain-"} + date + ".json").c_str(),
                  get("/v1/options/chain", std::string{"underlying=BTC&expiry="} + date)->body);
        }
        write("surface.json", fit->body);
    }
}
