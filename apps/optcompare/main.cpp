// reads two running option gateways and prints, per expiry, how the same contracts compare;
// a report, so the medians are doubles and nothing here decides anything; core is the left
// venue's 0.2 to 0.8 absolute delta; mid iv is implied here from each venue's mid on its own forward,
// so the venues' iv conventions drop out; the surface is fitted to both venues' quotes together, and a venue's
// residual is its mid iv minus the surface

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/options/chain.hpp>
#include <raikiri/options/pricing.hpp>
#include <raikiri/options/surface.hpp>
#include <raikiri/shm/records.hpp>
#include <raikiri/shm/segment.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using namespace raikiri;

struct Side {
    std::string name;
    shm::SegmentClient client;
    shm::GatewayChannels channels;
    std::vector<Instrument> catalogue;
};

struct Quote {
    std::optional<double> iv;
    std::optional<double> mid_iv;
    std::optional<double> delta;
    std::optional<double> mid_usd;
    std::optional<double> spread_usd;
    std::optional<double> mark_usd;
    std::optional<options::MarketQuote> market;
};

std::optional<Side> attach(const std::string& name) {
    const auto account = account_from_string(name);
    if (account.venue == VenueId::Invalid) {
        fmt::print(stderr, "optcompare: {} is not an account name\n", name);
        return std::nullopt;
    }
    auto client = shm::SegmentClient::attach(shm::gateway_segment_path(account));
    if (!client.has_value()) {
        fmt::print(stderr, "optcompare: no live segment for {}\n", name);
        return std::nullopt;
    }
    auto channels = shm::open_gateway_channels(*client);
    if (!channels.has_value()) {
        fmt::print(stderr, "optcompare: {} has not published its channels\n", name);
        return std::nullopt;
    }
    const auto all = channels->catalogue->all();
    return Side{.name = name,
                .client = std::move(*client),
                .channels = *channels,
                .catalogue = std::vector<Instrument>{all.begin(), all.end()}};
}

double as_double(FixedE9 value) {
    return static_cast<double>(value.raw) / 1e9;
}

std::optional<double> defined(FixedE9 value) {
    return value.is_undef() ? std::nullopt : std::optional{as_double(value)};
}

UnixNanos wall_now() {
    return UnixNanos{static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count())};
}

// a coin premium is the usd price over the forward, so it turns into usd at the venue's forward
Quote quote_of(const Side& side, InstrumentId id, const options::OptionKey& key, UnixNanos now) {
    Quote out{};
    const auto* instrument = side.channels.catalogue->find(id);
    const auto index = id.symbol().raw;
    if (instrument == nullptr || index >= shm::k_greeks_cells) {
        return out;
    }
    const auto& option = std::get<OptionInstrument>(*instrument);
    OptionGreeks greeks{};
    const bool priced = side.channels.greeks->read(index, greeks) != 0 && greeks.instrument == id;
    const auto forward = !priced                               ? FixedE9::undef()
                         : !greeks.underlying_price.is_undef() ? greeks.underlying_price
                                                               : greeks.index_price;
    double scale = 1.0;
    if (option.premium == OptionUnit::Base) {
        const auto usd = defined(forward);
        if (!usd.has_value()) {
            return out;
        }
        scale = *usd;
    }
    if (priced) {
        out.iv = defined(greeks.mark_iv);
        out.delta = defined(greeks.delta);
        if (const auto mark = defined(greeks.mark_price); mark.has_value()) {
            out.mark_usd = *mark * scale;
        }
    }
    Bbo bbo{};
    if (side.channels.bbo->read(index, bbo) != 0 && bbo.instrument == id && !bbo.bid.is_undef() &&
        !bbo.ask.is_undef()) {
        const auto bid = option.base.ticks_to_price(bbo.bid);
        const auto ask = option.base.ticks_to_price(bbo.ask);
        if (bid.is_undef() || ask.is_undef()) {
            return out;
        }
        if (!forward.is_undef()) {
            out.market = options::MarketQuote{.venue = id.venue(),
                                              .strike = key.strike,
                                              .put_call = key.put_call,
                                              .bid = bid,
                                              .ask = ask,
                                              .unit = option.premium,
                                              .forward = forward,
                                              .ts = bbo.ts_init};
        }
        out.mid_usd = (as_double(bid) + as_double(ask)) / 2.0 * scale;
        out.spread_usd = (as_double(ask) - as_double(bid)) * scale;
        out.mid_iv = defined(options::implied_vol({.price = FixedE9::from_raw(bid.raw + (ask.raw - bid.raw) / 2),
                                                   .forward = forward,
                                                   .strike = key.strike,
                                                   .years = options::years_between(now, key.expiry),
                                                   .put_call = key.put_call,
                                                   .unit = option.premium}));
    }
    return out;
}

double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::ranges::sort(values);
    const auto mid = values.size() / 2;
    return values.size() % 2 == 1 ? values[mid] : (values[mid - 1] + values[mid]) / 2.0;
}

struct Bucket {
    std::size_t matched{0};
    std::vector<double> iv_diff;
    std::vector<double> iv_diff_core;
    std::vector<double> mid_iv_diff;
    std::vector<double> mid_diff;
    std::vector<double> mark_diff;
    std::vector<double> spread_left;
    std::vector<double> spread_right;
    std::vector<double> fit_left;
    std::vector<double> fit_right;
};

using Slices = std::map<std::string, std::map<std::uint64_t, options::SliceInput>>;

void collect(const Side& side, const options::OptionChain& chain, UnixNanos now, Slices& slices) {
    for (const auto& underlying : chain.underlyings()) {
        auto& by_expiry = slices[std::string{underlying.underlying.view()}];
        for (const auto& expiry : underlying.expiries) {
            auto& slice = by_expiry[expiry.expiry.ns];
            slice.expiry = expiry.expiry;
            for (const auto& row : expiry.rows) {
                for (const auto& [id, pc] : {std::pair{row.call, PutCall::Call}, std::pair{row.put, PutCall::Put}}) {
                    if (!id.has_value()) {
                        continue;
                    }
                    const options::OptionKey key{.underlying = underlying.underlying,
                                                 .expiry = expiry.expiry,
                                                 .strike = row.strike,
                                                 .put_call = pc};
                    if (auto q = quote_of(side, *id, key, now); q.market.has_value()) {
                        slice.quotes.push_back(*q.market);
                    }
                }
            }
        }
    }
}

std::optional<double> fitted(const std::map<std::string, options::Surface>& surfaces, const options::OptionKey& key) {
    const auto found = surfaces.find(std::string{key.underlying.view()});
    if (found == surfaces.end()) {
        return std::nullopt;
    }
    const auto at = options::surface_at(
        found->second, key.strike, options::years_between(found->second.as_of, key.expiry));
    return at.has_value() ? defined(at->vol) : std::nullopt;
}

std::string_view model_name(const options::Surface& surface) {
    switch (surface.model) {
    case options::SurfaceModel::Ssvi:
        return "ssvi";
    case options::SurfaceModel::Svi:
        return surface.calendar_repair ? "svi, calendar repaired" : "svi";
    case options::SurfaceModel::None:
        break;
    }
    return "none";
}

void print_surface(const std::string& underlying, const options::Surface& surface) {
    const auto report = options::check_arbitrage(surface);
    fmt::print("\nsurface {}: {}, rmse {:.2f} vp (svi {:.2f}, ssvi {:.2f}), ssvi rho {:.3f} eta {:.3f} gamma {:.3f}\n"
               "  butterfly points {} (min g {:.4f}), calendar points {}, ssvi conditions {}\n",
               underlying,
               model_name(surface),
               as_double(surface.rmse) * 100.0,
               as_double(surface.svi_rmse) * 100.0,
               as_double(surface.ssvi_rmse) * 100.0,
               as_double(surface.ssvi.rho),
               as_double(surface.ssvi.eta),
               as_double(surface.ssvi.gamma),
               report.butterfly,
               as_double(report.min_density),
               report.calendar,
               report.ssvi_conditions ? "hold" : "fail");
    fmt::print("  {:<10} {:>6} {:>10} {:>5} {:>7} {:>8} {:>8} {:>7}\n",
               "expiry",
               "days",
               "forward",
               "n",
               "dropped",
               "svi vp",
               "ssvi vp",
               "inside");
    for (const auto& slice : surface.slices) {
        const auto name = options::canonical_name(options::OptionKey{.expiry = slice.expiry});
        fmt::print("  {:<10} {:>6.1f} {:>10.2f} {:>5} {:>7} {:>8.2f} {:>8.2f} {:>6.0f}%\n",
                   name.substr(1, 8),
                   as_double(slice.years) * 365.0,
                   as_double(slice.forward),
                   slice.quotes,
                   slice.dropped,
                   as_double(slice.svi_rmse) * 100.0,
                   as_double(slice.ssvi_rmse) * 100.0,
                   as_double(slice.inside) * 100.0);
    }
    for (const auto& venue : surface.venues) {
        fmt::print("  {:<10} n {:>4}  mid iv minus surface {:>+6.2f} vp, rms {:.2f} vp\n",
                   to_string(venue.venue),
                   venue.quotes,
                   as_double(venue.mean) * 100.0,
                   as_double(venue.rms) * 100.0);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3 || std::string_view{argv[1]} == "--help") {
        fmt::print("usage: optcompare <left-account> <right-account> [--rows]\n"
                   "  e.g. optcompare deribit-opt derive-opt; differences are right minus left\n");
        return argc < 3 ? 2 : 0;
    }
    const bool rows = argc > 3 && std::string_view{argv[3]} == "--rows";
    auto left = attach(argv[1]);
    auto right = attach(argv[2]);
    if (!left.has_value() || !right.has_value()) {
        return 1;
    }
    const auto left_chain = options::OptionChain::build(left->catalogue);
    const auto right_chain = options::OptionChain::build(right->catalogue);
    const auto matched = options::match(left_chain, right_chain);
    fmt::print("{}: {} options, {}: {} options, {} contracts on both\n",
               left->name,
               left_chain.size(),
               right->name,
               right_chain.size(),
               matched.size());

    const auto now = wall_now();
    Slices slices;
    collect(*left, left_chain, now, slices);
    collect(*right, right_chain, now, slices);
    std::map<std::string, options::Surface> surfaces;
    for (const auto& [underlying, by_expiry] : slices) {
        std::vector<options::SliceInput> inputs;
        for (const auto& [expiry, slice] : by_expiry) {
            inputs.push_back(slice);
        }
        // a quiet book keeps its last quote, so stale means minutes here
        auto surface = options::fit_surface(inputs, now, {.stale_ns = 600'000'000'000ULL});
        if (surface.model != options::SurfaceModel::None) {
            surfaces.emplace(underlying, std::move(surface));
        }
    }

    std::map<std::tuple<std::string, std::uint64_t>, Bucket> buckets;
    for (const auto& pair : matched) {
        const auto a = quote_of(*left, pair.left, pair.key, now);
        const auto b = quote_of(*right, pair.right, pair.key, now);
        auto& bucket = buckets[{std::string{pair.key.underlying.view()}, pair.key.expiry.ns}];
        ++bucket.matched;
        if (a.iv.has_value() && b.iv.has_value()) {
            bucket.iv_diff.push_back((*b.iv - *a.iv) * 100.0);
            if (a.delta.has_value() && std::abs(*a.delta) >= 0.2 && std::abs(*a.delta) <= 0.8) {
                bucket.iv_diff_core.push_back((*b.iv - *a.iv) * 100.0);
            }
        }
        if (a.mid_iv.has_value() && b.mid_iv.has_value()) {
            bucket.mid_iv_diff.push_back((*b.mid_iv - *a.mid_iv) * 100.0);
        }
        const auto fit = fitted(surfaces, pair.key);
        if (fit.has_value() && a.mid_iv.has_value()) {
            bucket.fit_left.push_back((*a.mid_iv - *fit) * 100.0);
        }
        if (fit.has_value() && b.mid_iv.has_value()) {
            bucket.fit_right.push_back((*b.mid_iv - *fit) * 100.0);
        }
        if (a.mark_usd.has_value() && b.mark_usd.has_value()) {
            bucket.mark_diff.push_back(*b.mark_usd - *a.mark_usd);
        }
        if (a.mid_usd.has_value() && b.mid_usd.has_value()) {
            bucket.mid_diff.push_back(*b.mid_usd - *a.mid_usd);
            bucket.spread_left.push_back(*a.spread_usd);
            bucket.spread_right.push_back(*b.spread_usd);
        }
        if (rows) {
            fmt::print("{:<26} iv {:>7.2f} {:>7.2f}  mid iv {:>7.2f} {:>7.2f}  fit {:>7.2f}  mark {:>10.2f} {:>10.2f}  "
                       "mid {:>10.2f} {:>10.2f}  spread {:>8.2f} {:>8.2f}\n",
                       options::canonical_name(pair.key),
                       a.iv.value_or(0.0) * 100.0,
                       b.iv.value_or(0.0) * 100.0,
                       a.mid_iv.value_or(0.0) * 100.0,
                       b.mid_iv.value_or(0.0) * 100.0,
                       fit.value_or(0.0) * 100.0,
                       a.mark_usd.value_or(0.0),
                       b.mark_usd.value_or(0.0),
                       a.mid_usd.value_or(0.0),
                       b.mid_usd.value_or(0.0),
                       a.spread_usd.value_or(0.0),
                       b.spread_usd.value_or(0.0));
        }
    }

    fmt::print("\n{:<6} {:<10} {:>7} {:>6} {:>10} {:>6} {:>10} {:>6} {:>10} {:>6} {:>11} {:>11} {:>10} {:>10} {:>9} "
               "{:>9}\n",
               "und",
               "expiry",
               "matched",
               "iv n",
               "d iv vp",
               "core n",
               "d iv core",
               "miv n",
               "d mid iv",
               "mid n",
               "d mark usd",
               "d mid usd",
               "sprd left",
               "sprd rght",
               "fit left",
               "fit rght");
    for (const auto& [key, bucket] : buckets) {
        const auto& [underlying, expiry] = key;
        const options::OptionKey day{.expiry = UnixNanos{expiry}};
        const auto name = options::canonical_name(day);
        fmt::print(
            "{:<6} {:<10} {:>7} {:>6} {:>10.2f} {:>6} {:>10.2f} {:>6} {:>10.2f} {:>6} {:>11.2f} {:>11.2f} {:>10.2f} "
            "{:>10.2f} {:>9.2f} {:>9.2f}\n",
            underlying,
            name.substr(1, 8),
            bucket.matched,
            bucket.iv_diff.size(),
            median(bucket.iv_diff),
            bucket.iv_diff_core.size(),
            median(bucket.iv_diff_core),
            bucket.mid_iv_diff.size(),
            median(bucket.mid_iv_diff),
            bucket.mid_diff.size(),
            median(bucket.mark_diff),
            median(bucket.mid_diff),
            median(bucket.spread_left),
            median(bucket.spread_right),
            median(bucket.fit_left),
            median(bucket.fit_right));
    }
    for (const auto& [underlying, surface] : surfaces) {
        print_surface(underlying, surface);
    }
    return 0;
}
