#include <raikiri/optiondata/board.hpp>
#include <raikiri/options/pricing.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string>

namespace raikiri::optiondata {

namespace internal {

[[nodiscard]] bool defined(FixedE9 value) noexcept {
    return !value.is_undef();
}

[[nodiscard]] bool positive(FixedE9 value) noexcept {
    return defined(value) && value.raw > 0;
}

[[nodiscard]] FixedE9 narrow(int128_t value) noexcept {
    if (value > std::numeric_limits<std::int64_t>::max() || value <= std::numeric_limits<std::int64_t>::min()) {
        return FixedE9::undef();
    }
    return FixedE9::from_raw(static_cast<std::int64_t>(value));
}

[[nodiscard]] FixedE9 mul(FixedE9 a, FixedE9 b) noexcept {
    if (!defined(a) || !defined(b)) {
        return FixedE9::undef();
    }
    return narrow(static_cast<int128_t>(a.raw) * b.raw / k_e9);
}

[[nodiscard]] FixedE9 sub(FixedE9 a, FixedE9 b) noexcept {
    return defined(a) && defined(b) ? a - b : FixedE9::undef();
}

[[nodiscard]] FixedE9 mid(FixedE9 a, FixedE9 b) noexcept {
    if (!defined(a) || !defined(b)) {
        return FixedE9::undef();
    }
    return narrow((static_cast<int128_t>(a.raw) + b.raw) / 2);
}

[[nodiscard]] FixedE9 median(std::vector<FixedE9> values) {
    std::erase_if(values, [](FixedE9 v) { return !defined(v); });
    if (values.empty()) {
        return FixedE9::undef();
    }
    std::ranges::sort(values);
    const auto half = values.size() / 2;
    return values.size() % 2 == 1 ? values[half] : mid(values[half - 1], values[half]);
}

[[nodiscard]] OptionUnit pricing_unit(OptionUnit premium) noexcept {
    return premium == OptionUnit::Base ? OptionUnit::Base : OptionUnit::Quote;
}

[[nodiscard]] FixedE9
implied(FixedE9 price, const options::OptionKey& key, OptionUnit premium, FixedE9 forward, FixedE9 years) noexcept {
    if (!positive(price) || !positive(forward) || !positive(years)) {
        return FixedE9::undef();
    }
    return options::implied_vol({.price = price,
                                 .forward = forward,
                                 .strike = key.strike,
                                 .years = years,
                                 .put_call = key.put_call,
                                 .unit = pricing_unit(premium)});
}

[[nodiscard]] FixedE9 venue_figure(const std::optional<OptionGreeks>& greeks, FixedE9 OptionGreeks::*field) noexcept {
    return greeks.has_value() ? (*greeks).*field : FixedE9::undef();
}

struct VenueFigures {
    std::vector<FixedE9> index;
    std::vector<FixedE9> underlying;
};

[[nodiscard]] std::map<VenueId, VenueRef> venue_refs(std::span<const OptionQuote> quotes) {
    std::map<VenueId, VenueFigures> figures;
    for (const auto& q : quotes) {
        auto& f = figures[q.instrument.venue()];
        f.index.push_back(venue_figure(q.greeks, &OptionGreeks::index_price));
        f.underlying.push_back(venue_figure(q.greeks, &OptionGreeks::underlying_price));
    }
    std::map<VenueId, VenueRef> out;
    for (auto& [venue, f] : figures) {
        out.emplace(venue,
                    VenueRef{.venue = venue,
                             .index = median(std::move(f.index)),
                             .underlying = median(std::move(f.underlying))});
    }
    return out;
}

// the rate a coin premium turns into usd at: the venue's index, else its forward, else ours
[[nodiscard]] FixedE9 rate_of(const VenueRef& ref, FixedE9 forward) noexcept {
    if (positive(ref.index)) {
        return ref.index;
    }
    return positive(ref.underlying) ? ref.underlying : forward;
}

[[nodiscard]] FixedE9 usd(FixedE9 price, OptionUnit premium, FixedE9 rate) noexcept {
    return premium == OptionUnit::Base ? mul(price, rate) : price;
}

[[nodiscard]] QuoteView view_of(const OptionQuote& q, const VenueRef& ref, FixedE9 forward, FixedE9 years) {
    QuoteView v{.venue = q.instrument.venue(),
                .instrument = q.instrument,
                .symbol = q.symbol,
                .premium = q.premium,
                .bid = q.bid,
                .ask = q.ask,
                .bid_size = q.bid_size,
                .ask_size = q.ask_size,
                .open_interest = q.open_interest,
                .volume_24h = q.volume_24h,
                .ts = q.ts};
    const auto venue_mark = venue_figure(q.greeks, &OptionGreeks::mark_price);
    v.mark = defined(venue_mark) ? venue_mark : mid(q.bid, q.ask);
    const auto rate = rate_of(ref, forward);
    v.bid_usd = usd(v.bid, q.premium, rate);
    v.ask_usd = usd(v.ask, q.premium, rate);
    v.mark_usd = usd(v.mark, q.premium, rate);

    const auto venue_iv = venue_figure(q.greeks, &OptionGreeks::mark_iv);
    const auto venue_bid_iv = venue_figure(q.greeks, &OptionGreeks::bid_iv);
    const auto venue_ask_iv = venue_figure(q.greeks, &OptionGreeks::ask_iv);
    v.bid_iv = defined(venue_bid_iv) ? venue_bid_iv : implied(q.bid, q.key, q.premium, forward, years);
    v.ask_iv = defined(venue_ask_iv) ? venue_ask_iv : implied(q.ask, q.key, q.premium, forward, years);
    if (defined(venue_iv)) {
        v.mark_iv = venue_iv;
        v.iv = Figure::Venue;
    } else {
        v.mark_iv = implied(v.mark, q.key, q.premium, forward, years);
        if (defined(v.mark_iv) || defined(v.bid_iv) || defined(v.ask_iv)) {
            v.iv = Figure::Model;
        }
    }

    if (defined(venue_figure(q.greeks, &OptionGreeks::delta))) {
        v.delta = q.greeks->delta;
        v.gamma = q.greeks->gamma;
        v.vega = q.greeks->vega;
        v.theta = q.greeks->theta;
        v.greeks = Figure::Venue;
        v.greeks_unit = q.greeks_unit;
    } else if (positive(v.mark_iv) && positive(forward) && positive(years)) {
        const auto model = options::black76(
            {.forward = forward, .strike = q.key.strike, .vol = v.mark_iv, .years = years, .put_call = q.key.put_call});
        v.delta = model.delta;
        v.gamma = model.gamma;
        v.vega = model.vega;
        v.theta = model.theta;
        v.greeks = Figure::Model;
        v.greeks_unit = OptionUnit::Quote;
    }
    return v;
}

void rank(SideView& side) {
    for (const auto& v : side.venues) {
        if (positive(v.bid_usd) && (!defined(side.bid.usd) || v.bid_usd > side.bid.usd)) {
            side.bid = Best{.venue = v.venue, .usd = v.bid_usd, .iv = v.bid_iv, .size = v.bid_size};
        }
        if (positive(v.ask_usd) && (!defined(side.ask.usd) || v.ask_usd < side.ask.usd)) {
            side.ask = Best{.venue = v.venue, .usd = v.ask_usd, .iv = v.ask_iv, .size = v.ask_size};
        }
    }
}

[[nodiscard]] std::optional<FixedE9> vol_at(const options::Surface& surface, FixedE9 strike, FixedE9 years) noexcept {
    const auto at = options::surface_at(surface, strike, years);
    if (!at.has_value() || !positive(at->vol)) {
        return std::nullopt;
    }
    return at->vol;
}

} // namespace internal

std::string expiry_date(UnixNanos expiry) {
    // canonical_name gives -YYYYMMDD- for a key with no underlying
    return options::canonical_name(options::OptionKey{.expiry = expiry}).substr(1, 8);
}

std::vector<UnderlyingListing> summarise(std::span<const ListedOption> listed) {
    struct Expiry {
        std::set<std::int64_t> strikes;
        std::set<VenueId> venues;
    };

    struct Acc {
        UnderlyingText name;
        std::map<VenueId, std::uint32_t> venues;
        std::map<std::uint64_t, Expiry> expiries;
    };

    std::map<std::string, Acc, std::less<>> by_name;
    for (const auto& option : listed) {
        auto& acc = by_name[std::string{option.key.underlying.view()}];
        acc.name = option.key.underlying;
        ++acc.venues[option.venue];
        auto& expiry = acc.expiries[option.key.expiry.ns];
        expiry.strikes.insert(option.key.strike.raw);
        expiry.venues.insert(option.venue);
    }
    std::vector<UnderlyingListing> out;
    out.reserve(by_name.size());
    for (auto& [name, acc] : by_name) {
        UnderlyingListing row{.underlying = acc.name};
        for (const auto& [venue, count] : acc.venues) {
            row.venues.push_back({.venue = venue, .options = count});
        }
        for (const auto& [expiry, e] : acc.expiries) {
            row.expiries.push_back({.expiry = UnixNanos{expiry},
                                    .strikes = static_cast<std::uint32_t>(e.strikes.size()),
                                    .venues = {e.venues.begin(), e.venues.end()}});
        }
        out.push_back(std::move(row));
    }
    return out;
}

ExpiryForward expiry_forward(std::span<const OptionQuote> quotes) {
    struct Pair {
        FixedE9 call{FixedE9::undef()};
        FixedE9 put{FixedE9::undef()};
    };

    struct Venue {
        OptionUnit unit{OptionUnit::Quote};
        std::map<std::int64_t, Pair> strikes;
    };

    std::map<VenueId, Venue> venues;
    for (const auto& q : quotes) {
        const auto m = internal::mid(q.bid, q.ask);
        if (!internal::positive(q.bid) || !internal::positive(m)) {
            continue;
        }
        auto& venue = venues[q.instrument.venue()];
        venue.unit = internal::pricing_unit(q.premium);
        auto& pair = venue.strikes[q.key.strike.raw];
        (q.key.put_call == PutCall::Call ? pair.call : pair.put) = m;
    }
    std::vector<FixedE9> parity;
    std::vector<options::ParityQuote> pairs;
    for (const auto& [id, venue] : venues) {
        pairs.clear();
        for (const auto& [strike, pair] : venue.strikes) {
            if (internal::defined(pair.call) && internal::defined(pair.put)) {
                pairs.push_back({.strike = FixedE9::from_raw(strike), .call = pair.call, .put = pair.put});
            }
        }
        if (const auto forward = options::forward_from_parity(pairs, venue.unit); forward.has_value()) {
            parity.push_back(*forward);
        }
    }
    if (const auto value = internal::median(std::move(parity)); internal::positive(value)) {
        return {.value = value, .from = ForwardFrom::Parity};
    }
    std::vector<FixedE9> underlying;
    std::vector<FixedE9> index;
    for (const auto& q : quotes) {
        underlying.push_back(internal::venue_figure(q.greeks, &OptionGreeks::underlying_price));
        index.push_back(internal::venue_figure(q.greeks, &OptionGreeks::index_price));
    }
    if (const auto value = internal::median(std::move(underlying)); internal::positive(value)) {
        return {.value = value, .from = ForwardFrom::Venue};
    }
    if (const auto value = internal::median(std::move(index)); internal::positive(value)) {
        return {.value = value, .from = ForwardFrom::Index};
    }
    return {};
}

FixedE9 atm_strike(std::span<const FixedE9> strikes, FixedE9 forward) noexcept {
    if (!internal::positive(forward)) {
        return FixedE9::undef();
    }
    auto best = FixedE9::undef();
    for (const auto strike : strikes) {
        const auto distance = strike > forward ? strike - forward : forward - strike;
        const auto best_distance = !internal::defined(best) ? distance
                                                            : (best > forward ? best - forward : forward - best);
        if (!internal::defined(best) || distance < best_distance) {
            best = strike;
        }
    }
    return best;
}

ChainView
build_chain(std::span<const OptionQuote> quotes, UnixNanos expiry, UnixNanos now, std::span<const VenueId> shown) {
    ChainView chain{.expiry = expiry, .as_of = now, .years = options::years_between(now, expiry)};
    if (!quotes.empty()) {
        chain.underlying = quotes.front().key.underlying;
    }
    chain.forward = expiry_forward(quotes);
    const auto refs = internal::venue_refs(quotes);
    const auto visible = [&shown](VenueId venue) { return shown.empty() || std::ranges::contains(shown, venue); };
    for (const auto& [venue, ref] : refs) {
        if (visible(venue)) {
            chain.venues.push_back(ref);
        }
    }
    std::map<std::int64_t, RowView> rows;
    for (const auto& q : quotes) {
        const auto venue = q.instrument.venue();
        if (!visible(venue)) {
            continue;
        }
        auto& row = rows[q.key.strike.raw];
        row.strike = q.key.strike;
        auto& side = q.key.put_call == PutCall::Call ? row.call : row.put;
        side.venues.push_back(internal::view_of(q, refs.at(venue), chain.forward.value, chain.years));
    }
    chain.rows.reserve(rows.size());
    std::vector<FixedE9> strikes;
    for (auto& [strike, row] : rows) {
        for (auto* side : {&row.call, &row.put}) {
            std::ranges::sort(side->venues, {}, &QuoteView::venue);
            internal::rank(*side);
        }
        strikes.push_back(row.strike);
        chain.rows.push_back(std::move(row));
    }
    chain.atm_strike = atm_strike(strikes, chain.forward.value);
    return chain;
}

FixedE9 delta_strike(const options::Surface& surface, FixedE9 forward, FixedE9 years, FixedE9 delta) noexcept {
    if (!internal::positive(forward) || !internal::positive(years) || !internal::defined(delta) || delta.raw == 0) {
        return FixedE9::undef();
    }
    const bool call = delta.raw > 0;
    // forward delta falls with the strike on either side, so a bisection finds it
    const auto excess = [&](FixedE9 strike) -> std::optional<std::int64_t> {
        const auto vol = internal::vol_at(surface, strike, years);
        if (!vol.has_value()) {
            return std::nullopt;
        }
        const auto model = options::black76({.forward = forward,
                                             .strike = strike,
                                             .vol = *vol,
                                             .years = years,
                                             .put_call = call ? PutCall::Call : PutCall::Put});
        if (!internal::defined(model.delta)) {
            return std::nullopt;
        }
        return model.delta.raw - delta.raw;
    };
    auto lo = call ? forward : FixedE9::from_raw(forward.raw / 4);
    auto hi = call ? forward.scaled_by(4) : forward;
    const auto at_lo = excess(lo);
    const auto at_hi = excess(hi);
    if (!at_lo.has_value() || !at_hi.has_value() || *at_lo < 0 || *at_hi > 0) {
        return FixedE9::undef();
    }
    constexpr int k_steps = 60;
    for (int i = 0; i < k_steps && hi.raw - lo.raw > 1; ++i) {
        const auto m = internal::mid(lo, hi);
        const auto at = excess(m);
        if (!at.has_value()) {
            return FixedE9::undef();
        }
        (*at > 0 ? lo : hi) = m;
    }
    return internal::mid(lo, hi);
}

namespace internal {

struct ExpiryQuotes {
    std::vector<OptionQuote> quotes;
    ExpiryForward forward;
    std::map<VenueId, FixedE9> venue_forward;
};

[[nodiscard]] std::vector<SmileQuote> smile_of(const ExpiryQuotes& e, FixedE9 years) {
    std::vector<SmileQuote> out;
    if (!positive(e.forward.value)) {
        return out;
    }
    for (const auto& q : e.quotes) {
        const bool otm = q.key.put_call == PutCall::Call ? q.key.strike >= e.forward.value
                                                         : q.key.strike < e.forward.value;
        const auto found = e.venue_forward.find(q.instrument.venue());
        if (!otm || found == e.venue_forward.end()) {
            continue;
        }
        const auto forward = found->second;
        const auto mid_iv = implied(mid(q.bid, q.ask), q.key, q.premium, forward, years);
        if (!defined(mid_iv)) {
            continue;
        }
        out.push_back({.venue = q.instrument.venue(),
                       .strike = q.key.strike,
                       .put_call = q.key.put_call,
                       .mid_iv = mid_iv,
                       .bid_iv = implied(q.bid, q.key, q.premium, forward, years),
                       .ask_iv = implied(q.ask, q.key, q.premium, forward, years)});
    }
    std::ranges::sort(out, [](const SmileQuote& a, const SmileQuote& b) {
        return a.strike != b.strike ? a.strike < b.strike : a.venue < b.venue;
    });
    return out;
}

// one point per strike, the median across venues, for the smile when no fit holds
[[nodiscard]] std::vector<options::SmilePoint> merged(std::span<const SmileQuote> smile) {
    std::vector<options::SmilePoint> out;
    for (std::size_t i = 0; i < smile.size();) {
        std::vector<FixedE9> vols;
        auto j = i;
        for (; j < smile.size() && smile[j].strike == smile[i].strike; ++j) {
            vols.push_back(smile[j].mid_iv);
        }
        out.push_back({.strike = smile[i].strike, .vol = median(std::move(vols))});
        i = j;
    }
    return out;
}

void describe_fit(ExpiryView& view, const options::Surface& surface, const options::SliceFit& fit) {
    view.fit = fit;
    if (const auto atm = vol_at(surface, fit.forward, fit.years); atm.has_value()) {
        view.atm_iv = *atm;
    }
    view.call25_strike = delta_strike(surface, fit.forward, fit.years, FixedE9::from_raw(250'000'000));
    view.put25_strike = delta_strike(surface, fit.forward, fit.years, FixedE9::from_raw(-250'000'000));
    if (defined(view.call25_strike) && defined(view.put25_strike) && defined(view.atm_iv)) {
        const auto call = vol_at(surface, view.call25_strike, fit.years);
        const auto put = vol_at(surface, view.put25_strike, fit.years);
        if (call.has_value() && put.has_value()) {
            view.rr25 = *call - *put;
            view.bf25 = sub(mid(*call, *put), view.atm_iv);
        }
    }
    if (!view.smile.empty()) {
        const auto lo = view.smile.front().strike;
        const auto hi = view.smile.back().strike;
        const auto steps = static_cast<std::int64_t>(k_fitted_points - 1);
        for (std::int64_t i = 0; i <= steps && hi > lo; ++i) {
            const auto strike = FixedE9::from_raw(
                lo.raw + static_cast<std::int64_t>(static_cast<int128_t>(hi.raw - lo.raw) * i / steps));
            if (const auto vol = vol_at(surface, strike, fit.years); vol.has_value()) {
                view.fitted.push_back({.strike = strike, .vol = *vol});
            }
        }
    }
    std::map<VenueId, std::pair<std::uint32_t, std::int64_t>> sums;
    for (const auto& point : view.smile) {
        if (const auto vol = vol_at(surface, point.strike, fit.years); vol.has_value()) {
            auto& [n, total] = sums[point.venue];
            ++n;
            total += (point.mid_iv - *vol).raw;
        }
    }
    for (const auto& [venue, sum] : sums) {
        view.residuals.push_back({.venue = venue,
                                  .quotes = sum.first,
                                  .mean = FixedE9::from_raw(sum.second / static_cast<std::int64_t>(sum.first))});
    }
}

} // namespace internal

SurfaceView build_surface(std::span<const OptionQuote> quotes, UnixNanos now) {
    SurfaceView view{.as_of = now};
    if (!quotes.empty()) {
        view.underlying = quotes.front().key.underlying;
    }
    std::map<std::uint64_t, internal::ExpiryQuotes> by_expiry;
    for (const auto& q : quotes) {
        by_expiry[q.key.expiry.ns].quotes.push_back(q);
    }
    std::vector<options::SliceInput> inputs;
    inputs.reserve(by_expiry.size());
    for (auto& [expiry, e] : by_expiry) {
        e.forward = expiry_forward(e.quotes);
        for (const auto& [venue, ref] : internal::venue_refs(e.quotes)) {
            const auto forward = internal::positive(ref.underlying) ? ref.underlying : e.forward.value;
            if (internal::positive(forward)) {
                e.venue_forward.emplace(venue, forward);
            }
        }
        options::SliceInput slice{.expiry = UnixNanos{expiry}, .forward = e.forward.value};
        for (const auto& q : e.quotes) {
            const auto found = e.venue_forward.find(q.instrument.venue());
            if (found == e.venue_forward.end() || !internal::positive(q.bid) || !internal::positive(q.ask)) {
                continue;
            }
            slice.quotes.push_back({.venue = q.instrument.venue(),
                                    .strike = q.key.strike,
                                    .put_call = q.key.put_call,
                                    .bid = q.bid,
                                    .ask = q.ask,
                                    .unit = internal::pricing_unit(q.premium),
                                    .forward = found->second,
                                    .ts = q.ts});
        }
        inputs.push_back(std::move(slice));
    }
    const auto surface = options::fit_surface(inputs, now, {.stale_ns = k_surface_stale_ns});
    view.model = surface.model;
    if (surface.model != options::SurfaceModel::None) {
        const auto report = options::check_arbitrage(surface);
        view.rmse = surface.rmse;
        view.ssvi = surface.ssvi;
        view.calendar_repair = surface.calendar_repair;
        view.butterfly_points = report.butterfly;
        view.calendar_points = report.calendar;
        view.venues = surface.venues;
    }
    for (const auto& [expiry, e] : by_expiry) {
        ExpiryView ev{
            .expiry = UnixNanos{expiry}, .years = options::years_between(now, UnixNanos{expiry}), .forward = e.forward};
        ev.smile = internal::smile_of(e, ev.years);
        const auto fit = std::ranges::find(surface.slices, UnixNanos{expiry}, &options::SliceFit::expiry);
        if (surface.model != options::SurfaceModel::None && fit != surface.slices.end()) {
            internal::describe_fit(ev, surface, *fit);
        } else if (internal::positive(e.forward.value)) {
            const auto points = internal::merged(ev.smile);
            if (const auto at = options::smile_at(points, e.forward.value); at.has_value()) {
                ev.atm_iv = at->vol;
            }
        }
        view.expiries.push_back(std::move(ev));
    }
    return view;
}

} // namespace raikiri::optiondata
