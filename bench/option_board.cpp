#include <raikiri/optiondata/queries.hpp>
#include <raikiri/optiondata/routes.hpp>

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// a book the size of btc across six venues: fourteen expiries of a hundred strikes each

namespace {

using raikiri::FixedE9;
using raikiri::InstrumentId;
using raikiri::MarketType;
using raikiri::OptionUnit;
using raikiri::PutCall;
using raikiri::SymbolId;
using raikiri::SymbolText;
using raikiri::UnderlyingText;
using raikiri::UnixNanos;
using raikiri::VenueId;
using raikiri::optiondata::GetChain;
using raikiri::optiondata::GetSurface;
using raikiri::optiondata::ListedOption;
using raikiri::optiondata::ListUnderlyings;
using raikiri::optiondata::OptionMarket;
using raikiri::optiondata::OptionQuote;
using raikiri::optiondata::OptionRoutes;
using raikiri::optiondata::Refit;
using raikiri::optiondata::UnderlyingListing;

constexpr UnixNanos k_now{1'793'606'400'000'000'000ULL};
constexpr std::uint64_t k_day_ns = 86'400ULL * 1'000'000'000ULL;
constexpr double k_spot = 100'000.0;

FixedE9 e9(double value) {
    return FixedE9::from_raw(std::llround(value * 1e9));
}

double cdf(double x) {
    return 0.5 * std::erfc(-x / std::sqrt(2.0));
}

std::vector<OptionQuote> board(int strikes_per_expiry) {
    std::vector<OptionQuote> out;
    std::uint32_t symbol = 1;
    const VenueId venues[]{
        VenueId::Deribit, VenueId::Derive, VenueId::Okx, VenueId::Bybit, VenueId::Binance, VenueId::Gate};
    const int days[]{1, 2, 3, 7, 14, 21, 30, 60, 90, 120, 180, 240, 300, 365};
    int salt = 0;
    for (const auto venue : venues) {
        const double bias = 0.004 * (++salt - 3);
        for (const int d : days) {
            const UnixNanos expiry{k_now.ns + static_cast<std::uint64_t>(d) * k_day_ns};
            const double years = d / 365.0;
            const double forward = k_spot * (1.0 + 0.0002 * d);
            for (int n = 0; n < strikes_per_expiry; ++n) {
                const double strike = forward * (0.5 + (1.5 * n) / strikes_per_expiry);
                const double rounded = std::round(strike / 500.0) * 500.0;
                const double k = std::log(rounded / forward);
                const double vol = 0.5 + 0.05 * std::sqrt(years) - 0.15 * k + 0.4 * k * k + bias;
                const double sd = vol * std::sqrt(years);
                const double d1 = (std::log(forward / rounded) + 0.5 * sd * sd) / sd;
                for (const auto pc : {PutCall::Call, PutCall::Put}) {
                    const double call = forward * cdf(d1) - rounded * cdf(d1 - sd);
                    const double usd = std::max(1.0, pc == PutCall::Call ? call : call - forward + rounded);
                    const double half = std::max(usd * 0.01, 1.0);
                    OptionQuote q{.instrument = InstrumentId::make(venue, MarketType::Option, SymbolId{symbol++}),
                                  .key = {.underlying = *UnderlyingText::from("BTC"),
                                          .expiry = expiry,
                                          .strike = e9(rounded),
                                          .put_call = pc},
                                  .symbol = *SymbolText::from("BTC-OPT"),
                                  .premium = OptionUnit::Quote,
                                  .greeks_unit = OptionUnit::Quote,
                                  .bid = e9(usd - half),
                                  .ask = e9(usd + half),
                                  .bid_size = e9(1.5),
                                  .ask_size = e9(2.0),
                                  .open_interest = e9(120.0),
                                  .volume_24h = e9(15.0),
                                  .ts = k_now};
                    out.push_back(q);
                }
            }
        }
    }
    return out;
}

class Fixed final : public OptionMarket {
public:
    explicit Fixed(std::vector<OptionQuote> quotes) : quotes_{std::move(quotes)} {
        std::vector<ListedOption> listed;
        listed.reserve(quotes_.size());
        for (const auto& q : quotes_) {
            listed.push_back({.venue = q.instrument.venue(), .key = q.key});
        }
        listing_ = raikiri::optiondata::summarise(listed);
    }

    [[nodiscard]] std::vector<UnderlyingListing> underlyings() const override { return listing_; }

    void read(std::string_view, std::optional<UnixNanos> expiry, std::vector<OptionQuote>& out) const override {
        out.clear();
        for (const auto& q : quotes_) {
            if (!expiry.has_value() || q.key.expiry == *expiry) {
                out.push_back(q);
            }
        }
    }

private:
    std::vector<OptionQuote> quotes_;
    std::vector<UnderlyingListing> listing_;
};

void bm_chain_use_case(benchmark::State& state) {
    const Fixed market{board(static_cast<int>(state.range(0)))};
    const GetChain chain{market, [] { return k_now; }};
    const std::string expiry = std::to_string(k_now.ns + 30 * k_day_ns);
    for (auto _ : state) {
        benchmark::DoNotOptimize(chain({.underlying = "BTC", .expiry = expiry}));
    }
}

BENCHMARK(bm_chain_use_case)->Arg(50)->Arg(100)->Unit(benchmark::kMicrosecond);

void bm_chain_route(benchmark::State& state) {
    const Fixed market{board(static_cast<int>(state.range(0)))};
    const auto clock = [] { return k_now; };
    const ListUnderlyings underlyings{market};
    const GetChain chain{market, clock};
    GetSurface surface{market, clock, Refit::Inline};
    const OptionRoutes routes{underlyings, chain, surface};
    const std::string query = "underlying=BTC&expiry=" + std::to_string(k_now.ns + 30 * k_day_ns);
    for (auto _ : state) {
        auto res = routes({.method = "GET", .path = "/v1/options/chain", .query = query});
        benchmark::DoNotOptimize(res);
    }
}

BENCHMARK(bm_chain_route)->Arg(50)->Arg(100)->Unit(benchmark::kMicrosecond);

void bm_surface_fit(benchmark::State& state) {
    const Fixed market{board(static_cast<int>(state.range(0)))};
    auto now = k_now;
    GetSurface surface{market, [&now] { return now; }, Refit::Inline, 1};
    for (auto _ : state) {
        now.ns += 1'000;
        benchmark::DoNotOptimize(surface("BTC", {}));
    }
}

BENCHMARK(bm_surface_fit)->Arg(25)->Arg(50)->Arg(100)->Unit(benchmark::kMillisecond)->Iterations(3);

} // namespace

BENCHMARK_MAIN();
