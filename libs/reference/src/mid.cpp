#include <raikiri/reference/mid.hpp>

#include <algorithm>
#include <limits>
#include <optional>

namespace raikiri::reference {

namespace internal {

std::optional<std::int64_t> scaled(std::int64_t a, std::int64_t b, std::int64_t d) noexcept {
    if (d == 0) {
        return std::nullopt;
    }
    const int128_t q = static_cast<int128_t>(a) * b / d;
    if (q > std::numeric_limits<std::int64_t>::max() || q <= std::numeric_limits<std::int64_t>::min()) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(q);
}

std::int64_t weighted_quantile(std::vector<Weighted>& points, std::int64_t num, std::int64_t den) {
    std::ranges::sort(points, {}, &Weighted::price);
    int128_t total = 0;
    for (const auto& p : points) {
        total += p.weight;
    }
    const int128_t target = total * num;
    int128_t cum = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        cum += points[i].weight;
        if (cum * den < target) {
            continue;
        }
        if (cum * den == target && i + 1 < points.size()) {
            return points[i].price + (points[i + 1].price - points[i].price) / 2;
        }
        return points[i].price;
    }
    return points.empty() ? 0 : points.back().price;
}

std::int64_t plain_median(std::vector<std::int64_t> values) {
    if (values.empty()) {
        return 0;
    }
    std::ranges::sort(values);
    const auto n = values.size();
    if (n % 2 == 1) {
        return values[n / 2];
    }
    return values[n / 2 - 1] + (values[n / 2] - values[n / 2 - 1]) / 2;
}

} // namespace internal

namespace {

using internal::Weighted;

constexpr std::int64_t k_one = k_e9;
// a stablecoin quoted outside this band is a broken book, not a depeg we could price through
constexpr std::int64_t k_stable_low = 800'000'000;
constexpr std::int64_t k_stable_high = 1'200'000'000;

struct Candidate {
    VenueId venue{VenueId::Invalid};
    MarketType market{MarketType::Spot};
    QuoteCcy ccy{QuoteCcy::Other};
    std::int64_t mid{0};
    std::int64_t half_spread{0};
    // usd before normalisation; zero when the venue did not say
    std::int64_t raw_weight{0};
    std::uint64_t age_ns{0};
    std::size_t state{0};
    std::int64_t price{0};
    std::int64_t weight{0};
    bool perp{false};
};

[[nodiscard]] bool is_perp(MarketType market) noexcept {
    return market == MarketType::Perp || market == MarketType::Future;
}

[[nodiscard]] std::int64_t abs64(std::int64_t v) noexcept {
    return v < 0 ? -v : v;
}

[[nodiscard]] std::optional<Cut> book_cut(const Quote& q, const Params& params, std::uint64_t now_ns) {
    if (q.bid.is_undef() || q.ask.is_undef() || q.bid.raw <= 0 || q.ask.raw <= 0) {
        return Cut::Empty;
    }
    if (q.bid.raw >= q.ask.raw) {
        return Cut::Crossed;
    }
    const auto mid = q.bid.raw + (q.ask.raw - q.bid.raw) / 2;
    if (internal::scaled(q.ask.raw - q.bid.raw, k_one, mid).value_or(params.max_spread_e9 + 1) > params.max_spread_e9) {
        return Cut::Wide;
    }
    const auto limit = is_perp(q.market) ? params.stale_perp_ns : params.stale_spot_ns;
    if (q.ts_ns == 0 || (now_ns > q.ts_ns && now_ns - q.ts_ns > limit)) {
        return Cut::Stale;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::int64_t> rate_of(QuoteCcy ccy, const StableRates& rates) noexcept {
    switch (ccy) {
    case QuoteCcy::Usd:
        return k_one;
    case QuoteCcy::Usdt:
        return rates.usdt_usd.raw;
    case QuoteCcy::Usdc:
        return rates.usdc_usd.raw;
    case QuoteCcy::Other:
        break;
    }
    return std::nullopt;
}

[[nodiscard]] std::int64_t raw_weight(const Quote& q, Weighting mode, std::int64_t rate) {
    if (mode == Weighting::Volume) {
        if (q.volume_24h.is_undef() || q.volume_24h.raw <= 0) {
            return 0;
        }
        return internal::scaled(q.volume_24h.raw, rate, k_e6 * k_one).value_or(0);
    }
    if (q.bid_qty.is_undef() || q.ask_qty.is_undef() || q.bid_qty.raw <= 0 || q.ask_qty.raw <= 0) {
        return 0;
    }
    constexpr std::int64_t k_e18 = k_one * k_one;
    const auto bid = internal::scaled(q.bid_qty.raw, q.bid.raw, k_e18).value_or(0);
    const auto ask = internal::scaled(q.ask_qty.raw, q.ask.raw, k_e18).value_or(0);
    return internal::scaled(std::min(bid, ask), rate, k_one).value_or(0);
}

// unknown weights take the median of the known ones, every weight is capped at a multiple of it
[[nodiscard]] std::vector<std::int64_t> weights_of(const std::vector<Candidate*>& set, const Params& params) {
    std::vector<std::int64_t> known;
    for (const auto* c : set) {
        if (c->raw_weight > 0) {
            known.push_back(c->raw_weight);
        }
    }
    const auto median = known.empty() ? std::int64_t{1} : std::max<std::int64_t>(internal::plain_median(known), 1);
    const auto cap = median * params.weight_cap;
    std::vector<std::int64_t> out;
    out.reserve(set.size());
    for (const auto* c : set) {
        auto w = std::min(c->raw_weight > 0 ? c->raw_weight : median, cap);
        if (c->perp) {
            w = mul_div(w, params.perp_weight_pct, 100);
        }
        out.push_back(std::max<std::int64_t>(w, 1));
    }
    return out;
}

[[nodiscard]] std::int64_t median_of(const std::vector<Candidate*>& set, std::int64_t Candidate::*field) {
    std::vector<Weighted> points;
    points.reserve(set.size());
    for (const auto* c : set) {
        points.push_back({.price = c->*field, .weight = c->weight});
    }
    return internal::weighted_quantile(points, 1, 2);
}

[[nodiscard]] std::size_t state_of(AssetState& state, VenueId venue, MarketType market) {
    for (std::size_t i = 0; i < state.sources.size(); ++i) {
        if (state.sources[i].venue == venue && state.sources[i].market == market) {
            return i;
        }
    }
    state.sources.push_back({.venue = venue, .market = market});
    return state.sources.size() - 1;
}

[[nodiscard]] std::vector<std::int64_t>
stable_mids(std::span<const Quote> books, QuoteCcy want, std::int64_t rate, const Params& params, std::uint64_t now) {
    std::vector<std::int64_t> out;
    for (const auto& q : books) {
        if (q.ccy != want || book_cut(q, params, now).has_value()) {
            continue;
        }
        const auto mid = internal::scaled(q.bid.raw + (q.ask.raw - q.bid.raw) / 2, rate, k_one).value_or(0);
        if (mid >= k_stable_low && mid <= k_stable_high) {
            out.push_back(mid);
        }
    }
    return out;
}

} // namespace

QuoteCcy quote_ccy_of(std::string_view local_symbol) noexcept {
    const auto slash = local_symbol.find('/');
    if (slash == std::string_view::npos) {
        return QuoteCcy::Other;
    }
    auto quote = local_symbol.substr(slash + 1);
    quote = quote.substr(0, quote.find_first_of("-:"));
    if (quote == "USD") {
        return QuoteCcy::Usd;
    }
    if (quote == "USDT") {
        return QuoteCcy::Usdt;
    }
    if (quote == "USDC") {
        return QuoteCcy::Usdc;
    }
    return QuoteCcy::Other;
}

std::string_view to_string(QuoteCcy ccy) noexcept {
    switch (ccy) {
    case QuoteCcy::Usd:
        return "usd";
    case QuoteCcy::Usdt:
        return "usdt";
    case QuoteCcy::Usdc:
        return "usdc";
    case QuoteCcy::Other:
        break;
    }
    return "other";
}

std::string_view to_string(Cut cut) noexcept {
    switch (cut) {
    case Cut::Empty:
        return "empty";
    case Cut::Crossed:
        return "crossed";
    case Cut::Wide:
        return "wide";
    case Cut::Stale:
        return "stale";
    case Cut::NoRate:
        return "no_rate";
    case Cut::Duplicate:
        return "duplicate";
    case Cut::Warming:
        return "warming";
    case Cut::Outlier:
        return "outlier";
    }
    return "unknown";
}

StableRates stable_rates(std::span<const Quote> usdt_books,
                         std::span<const Quote> usdc_books,
                         const Params& params,
                         std::uint64_t now_ns) {
    StableRates out{.usdt_usd = params.usdt_fallback, .usdc_usd = params.usdc_fallback};
    if (const auto usdt = stable_mids(usdt_books, QuoteCcy::Usd, k_one, params, now_ns); !usdt.empty()) {
        out.usdt_usd = FixedE9::from_raw(internal::plain_median(usdt));
        out.usdt_fallback = false;
        out.usdt_sources = static_cast<std::uint16_t>(usdt.size());
    }
    auto usdc = stable_mids(usdc_books, QuoteCcy::Usd, k_one, params, now_ns);
    if (!out.usdt_fallback) {
        const auto via = stable_mids(usdc_books, QuoteCcy::Usdt, out.usdt_usd.raw, params, now_ns);
        usdc.insert(usdc.end(), via.begin(), via.end());
    }
    if (!usdc.empty()) {
        out.usdc_usd = FixedE9::from_raw(internal::plain_median(usdc));
        out.usdc_fallback = false;
        out.usdc_sources = static_cast<std::uint16_t>(usdc.size());
    }
    return out;
}

Reference compute(std::span<const Quote> quotes,
                  const StableRates& rates,
                  const Params& params,
                  AssetState& state,
                  std::uint64_t now_ns) {
    Reference out{.quotes = static_cast<std::uint16_t>(quotes.size()), .rates = rates};
    const auto drop = [&out](VenueId venue, MarketType market, Cut cut) {
        ++out.cuts[static_cast<std::size_t>(cut)];
        out.cut.push_back({.venue = venue, .market = market, .cut = cut});
    };

    std::vector<Candidate> fresh;
    fresh.reserve(quotes.size());
    for (const auto& q : quotes) {
        if (const auto cut = book_cut(q, params, now_ns); cut.has_value()) {
            drop(q.venue, q.market, *cut);
            continue;
        }
        const auto rate = rate_of(q.ccy, rates);
        if (!rate.has_value()) {
            drop(q.venue, q.market, Cut::NoRate);
            continue;
        }
        const auto usd = internal::scaled(q.bid.raw + (q.ask.raw - q.bid.raw) / 2, *rate, k_one);
        const auto half = internal::scaled((q.ask.raw - q.bid.raw) / 2, *rate, k_one);
        if (!usd.has_value() || !half.has_value() || *usd <= 0) {
            drop(q.venue, q.market, Cut::NoRate);
            continue;
        }
        Candidate c{.venue = q.venue,
                    .market = q.market,
                    .ccy = q.ccy,
                    .mid = *usd,
                    .half_spread = *half,
                    .raw_weight = raw_weight(q, params.weighting, *rate),
                    .age_ns = now_ns > q.ts_ns ? now_ns - q.ts_ns : 0,
                    .perp = is_perp(q.market)};
        c.price = c.mid;
        // one book per venue and market, the busiest
        const auto twin = std::ranges::find_if(
            fresh, [&](const Candidate& held) { return held.venue == c.venue && held.market == c.market; });
        if (twin == fresh.end()) {
            fresh.push_back(c);
        } else if (c.raw_weight > twin->raw_weight) {
            drop(twin->venue, twin->market, Cut::Duplicate);
            *twin = c;
        } else {
            drop(c.venue, c.market, Cut::Duplicate);
        }
    }
    for (auto& c : fresh) {
        c.state = state_of(state, c.venue, c.market);
    }

    std::vector<Candidate*> judged;
    for (auto& c : fresh) {
        if (!c.perp) {
            judged.push_back(&c);
            continue;
        }
        const auto& s = state.sources[c.state];
        const bool warm = s.basis_since_ns != 0 && s.basis_at_ns - s.basis_since_ns >= params.basis_warm_ns &&
                          now_ns - s.basis_at_ns <= params.basis_tau_ns;
        if (!warm) {
            drop(c.venue, c.market, Cut::Warming);
            continue;
        }
        const auto adjusted = internal::scaled(c.mid, k_one, k_one + s.basis_e9);
        if (!adjusted.has_value() || *adjusted <= 0) {
            drop(c.venue, c.market, Cut::Warming);
            continue;
        }
        c.price = *adjusted;
        judged.push_back(&c);
    }

    std::vector<Candidate*> admitted;
    if (!judged.empty()) {
        std::vector<std::int64_t> prices;
        prices.reserve(judged.size());
        for (const auto* c : judged) {
            prices.push_back(c->price);
        }
        const auto centre = internal::plain_median(std::move(prices));
        for (auto* c : judged) {
            auto& s = state.sources[c->state];
            const auto dev = internal::scaled(abs64(c->price - centre), k_one, centre)
                                 .value_or(std::numeric_limits<std::int64_t>::max());
            if (!s.excluded && dev > params.outlier_e9) {
                s.excluded = true;
            } else if (s.excluded && dev < params.outlier_e9 / 2) {
                s.excluded = false;
            }
            if (s.excluded) {
                drop(c->venue, c->market, Cut::Outlier);
            } else {
                admitted.push_back(c);
            }
        }
    }

    const auto weights = weights_of(admitted, params);
    for (std::size_t i = 0; i < admitted.size(); ++i) {
        admitted[i]->weight = weights[i];
    }
    std::vector<Candidate*> spot;
    for (auto* c : admitted) {
        if (!c->perp) {
            spot.push_back(c);
        }
    }
    if (!spot.empty()) {
        out.spot_mid = FixedE9::from_raw(median_of(spot, &Candidate::price));
    }
    out.used_spot = static_cast<std::uint16_t>(spot.size());
    out.used_perp = static_cast<std::uint16_t>(admitted.size() - spot.size());

    // the perps' own level, screened against their own median since the basis is what it measures
    std::vector<Candidate*> perps;
    for (auto& c : fresh) {
        if (c.perp) {
            perps.push_back(&c);
        }
    }
    if (!perps.empty()) {
        std::vector<std::int64_t> mids;
        for (const auto* c : perps) {
            mids.push_back(c->mid);
        }
        const auto centre = internal::plain_median(std::move(mids));
        std::erase_if(perps, [&](const Candidate* c) {
            return internal::scaled(abs64(c->mid - centre), k_one, centre).value_or(params.outlier_e9 + 1) >
                   params.outlier_e9;
        });
        const auto perp_weights = weights_of(perps, params);
        std::vector<Weighted> points;
        for (std::size_t i = 0; i < perps.size(); ++i) {
            points.push_back({.price = perps[i]->mid, .weight = perp_weights[i]});
        }
        out.perp_mid = FixedE9::from_raw(internal::weighted_quantile(points, 1, 2));
    }

    // two spot books at least, or one bad book would become every perp's basis
    if (spot.size() >= 2) {
        for (auto& c : fresh) {
            if (!c.perp) {
                continue;
            }
            auto& s = state.sources[c.state];
            const auto ratio = internal::scaled(c.mid, k_one, out.spot_mid.raw);
            // half the price away is a different contract under the same asset key, not a basis
            if (!ratio.has_value() || abs64(*ratio - k_one) > k_one / 2) {
                s.basis_since_ns = 0;
                continue;
            }
            const auto x = *ratio - k_one;
            if (s.basis_since_ns == 0 || now_ns - s.basis_at_ns > params.basis_tau_ns) {
                s.basis_e9 = x;
                s.basis_since_ns = now_ns;
            } else {
                const auto dt = static_cast<std::int64_t>(now_ns - s.basis_at_ns);
                s.basis_e9 += mul_div(x - s.basis_e9, dt, static_cast<std::int64_t>(params.basis_tau_ns));
            }
            s.basis_at_ns = now_ns;
        }
    }

    for (const auto* c : admitted) {
        out.used.push_back({.venue = c->venue,
                            .market = c->market,
                            .ccy = c->ccy,
                            .price = FixedE9::from_raw(c->price),
                            .weight = c->weight});
        out.oldest_age_ns = std::max(out.oldest_age_ns, c->age_ns);
    }
    if (admitted.empty() || admitted.size() < params.min_sources) {
        return out;
    }

    out.refused = false;
    const auto mid = median_of(admitted, &Candidate::price);
    out.mid = FixedE9::from_raw(mid);

    std::vector<Weighted> points;
    for (const auto* c : admitted) {
        points.push_back({.price = c->price, .weight = c->weight});
    }
    const auto q25 = internal::weighted_quantile(points, 1, 4);
    const auto q75 = internal::weighted_quantile(points, 3, 4);
    const auto half_spread = median_of(admitted, &Candidate::half_spread);
    out.conf = FixedE9::from_raw(std::max((q75 - q25) / 2, half_spread));

    for (auto& p : points) {
        p.price = abs64(p.price - mid);
    }
    out.dispersion_e9 = FixedE9::from_raw(
        internal::scaled(internal::weighted_quantile(points, 1, 2), k_one, mid).value_or(k_undef_raw));
    if (const auto ratio = out.perp_mid.is_undef() ? std::nullopt : internal::scaled(out.perp_mid.raw, k_one, mid);
        ratio.has_value()) {
        out.basis_e9 = FixedE9::from_raw(*ratio - k_one);
    }
    return out;
}

} // namespace raikiri::reference
