#include <raikiri/optionrisk/risk.hpp>
#include <raikiri/options/pricing.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <tuple>

namespace raikiri::optionrisk {

namespace internal {

constexpr std::uint64_t k_day_ns = 86'400'000'000'000ULL;

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

[[nodiscard]] FixedE9 div(FixedE9 a, FixedE9 b) noexcept {
    if (!defined(a) || !defined(b) || b.raw == 0) {
        return FixedE9::undef();
    }
    return narrow(static_cast<int128_t>(a.raw) * k_e9 / b.raw);
}

[[nodiscard]] FixedE9 add(FixedE9 a, FixedE9 b) noexcept {
    if (!defined(a) || !defined(b)) {
        return FixedE9::undef();
    }
    return narrow(static_cast<int128_t>(a.raw) + b.raw);
}

[[nodiscard]] FixedE9 sub(FixedE9 a, FixedE9 b) noexcept {
    if (!defined(a) || !defined(b)) {
        return FixedE9::undef();
    }
    return narrow(static_cast<int128_t>(a.raw) - b.raw);
}

[[nodiscard]] FixedE9 mid(FixedE9 bid, FixedE9 ask) noexcept {
    if (!positive(bid) || !positive(ask)) {
        return FixedE9::undef();
    }
    return narrow((static_cast<int128_t>(bid.raw) + ask.raw) / 2);
}

[[nodiscard]] bool older_than(UnixNanos ts, UnixNanos now, std::uint64_t limit_ns) noexcept {
    return now.ns > ts.ns && now.ns - ts.ns > limit_ns;
}

[[nodiscard]] FixedE9 days_between(UnixNanos now, UnixNanos expiry) noexcept {
    const auto span = static_cast<int128_t>(expiry.ns) - static_cast<int128_t>(now.ns);
    return narrow(span * k_e9 / static_cast<int128_t>(k_day_ns));
}

[[nodiscard]] options::Black76
model(bool coin, FixedE9 forward, FixedE9 strike, FixedE9 vol, FixedE9 years, PutCall put_call) noexcept {
    const options::Black76Input in{
        .forward = forward, .strike = strike, .vol = vol, .years = years, .put_call = put_call};
    return coin ? options::inverse(in) : options::black76(in);
}

// sums what it can and says when a figure is missing
void accrue(FixedE9& into, FixedE9 value, bool& missing) noexcept {
    const auto sum = add(into, value);
    if (defined(sum)) {
        into = sum;
    } else {
        missing = true;
    }
}

void add_position(Totals& totals, const PositionRisk& p) noexcept {
    bool missing = (p.flags & (k_mark_missing | k_greeks_missing | k_entry_missing | k_rate_missing)) != 0U;
    accrue(totals.delta_coin, p.delta_coin, missing);
    accrue(totals.delta_adjusted_coin, p.delta_adjusted_coin, missing);
    accrue(totals.delta_usd, p.delta_usd, missing);
    accrue(totals.gamma, p.gamma, missing);
    accrue(totals.gamma_usd_1pct, p.gamma_usd_1pct, missing);
    accrue(totals.vega_usd, p.vega_usd, missing);
    accrue(totals.theta_usd, p.theta_usd, missing);
    accrue(totals.premium_usd, p.premium_usd, missing);
    accrue(totals.value_usd, p.value_usd, missing);
    accrue(totals.unrealized_usd, p.unrealized_usd, missing);
    ++totals.positions;
    if (missing) {
        ++totals.incomplete;
    }
}

void add_realized(Totals& totals, FixedE9 realized_usd) noexcept {
    bool missing = false;
    accrue(totals.realized_usd, realized_usd, missing);
    if (missing) {
        ++totals.incomplete;
    }
}

} // namespace internal

FixedE9 units_of(const Holding& holding) noexcept {
    return internal::mul(holding.contracts, holding.contract_value);
}

bool coin_settled(const Holding& holding) noexcept {
    return holding.premium == OptionUnit::Base;
}

FixedE9 times(FixedE9 a, FixedE9 b) noexcept {
    return internal::mul(a, b);
}

MarketState market_of(const optiondata::OptionQuote& quote, FixedE9 forward, FixedE9 rate) noexcept {
    MarketState out{.forward = forward, .rate = rate};
    if (quote.greeks.has_value()) {
        const auto& greeks = *quote.greeks;
        out.greeks_ts = greeks.ts_event;
        out.iv = internal::positive(greeks.mark_iv) ? greeks.mark_iv : FixedE9::undef();
        if (internal::positive(greeks.underlying_price)) {
            out.forward = greeks.underlying_price;
        }
        if (internal::positive(greeks.index_price)) {
            out.rate = greeks.index_price;
        }
        if (internal::positive(greeks.mark_price)) {
            out.mark = greeks.mark_price;
            out.mark_from = MarkFrom::Venue;
            out.mark_ts = greeks.ts_event;
        }
    }
    if (out.mark_from == MarkFrom::None) {
        out.mark = internal::mid(quote.bid, quote.ask);
        if (internal::defined(out.mark)) {
            out.mark_from = MarkFrom::Mid;
            out.mark_ts = quote.ts;
        }
    }
    return out;
}

PositionRisk
assess(const Holding& holding, const MarketState& market, FixedE9 realized, UnixNanos now, const Staleness& staleness) {
    PositionRisk out{.holding = holding, .market = market, .realized = realized};
    const bool coin = coin_settled(holding);
    const bool known_unit = holding.premium != OptionUnit::Unknown;
    out.units = units_of(holding);
    out.years = options::years_between(now, holding.key.expiry);
    out.days_to_expiry = internal::days_between(now, holding.key.expiry);
    if (internal::older_than(holding.as_of, now, staleness.position_ns)) {
        out.flags |= k_position_stale;
    }

    auto mark = known_unit ? market.mark : FixedE9::undef();
    const auto rate = coin ? market.rate : FixedE9::from_raw(k_e9);
    if (internal::positive(market.forward) && internal::positive(out.years)) {
        if (internal::positive(mark)) {
            out.vol = options::implied_vol({.price = mark,
                                            .forward = market.forward,
                                            .strike = holding.key.strike,
                                            .years = out.years,
                                            .put_call = holding.key.put_call,
                                            .unit = coin ? OptionUnit::Base : OptionUnit::Quote});
        }
        // at or below intrinsic no vol reprices the mark
        if (!internal::positive(out.vol)) {
            out.vol = market.iv;
        }
    }

    if (internal::positive(out.vol)) {
        const auto usd = options::black76({.forward = market.forward,
                                           .strike = holding.key.strike,
                                           .vol = out.vol,
                                           .years = out.years,
                                           .put_call = holding.key.put_call});
        const auto own = coin ? options::inverse({.forward = market.forward,
                                                  .strike = holding.key.strike,
                                                  .vol = out.vol,
                                                  .years = out.years,
                                                  .put_call = holding.key.put_call})
                              : usd;
        out.model_price = own.price;
        out.delta_coin = internal::mul(out.units, usd.delta);
        out.delta_adjusted_coin = internal::mul(out.units, own.delta);
        out.delta_usd = internal::mul(out.delta_coin, market.forward);
        out.gamma = internal::mul(out.units, usd.gamma);
        // one forward move of 1% changes delta_usd by gamma × 1%·F × F
        out.gamma_usd_1pct = internal::div(internal::mul(internal::mul(out.gamma, market.forward), market.forward),
                                           FixedE9::from_raw(100 * k_e9));
        out.vega_usd = internal::mul(out.units, usd.vega);
        out.theta_usd = internal::mul(out.units, usd.theta);
    }
    if (!internal::defined(out.delta_coin)) {
        out.flags |= k_greeks_missing;
    } else if (market.greeks_ts.ns != 0 && internal::older_than(market.greeks_ts, now, staleness.greeks_ns)) {
        out.flags |= k_greeks_stale;
    } else if (market.greeks_ts.ns == 0 && internal::older_than(market.mark_ts, now, staleness.greeks_ns)) {
        out.flags |= k_greeks_stale; // greeks implied off a mark inherit its age
    }

    if (!internal::positive(mark) && market.mark_from == MarkFrom::None && known_unit &&
        internal::positive(out.model_price)) {
        mark = out.model_price;
        out.market.mark = mark;
        out.market.mark_from = MarkFrom::Model;
        out.market.mark_ts = market.greeks_ts;
        out.flags |= k_mark_model;
    }
    if (!internal::defined(mark)) {
        out.market.mark = FixedE9::undef();
        out.market.mark_from = MarkFrom::None;
        out.flags |= k_mark_missing;
    } else if (internal::older_than(out.market.mark_ts, now, staleness.mark_ns)) {
        out.flags |= k_mark_stale;
    }
    if (!internal::positive(rate)) {
        out.flags |= k_rate_missing;
    }

    out.value = internal::mul(out.units, mark);
    out.value_usd = internal::mul(out.value, rate);
    if (internal::positive(holding.entry)) {
        out.premium = internal::mul(out.units, holding.entry);
        out.premium_usd = internal::mul(out.premium, rate);
        out.unrealized = internal::sub(out.value, out.premium);
        out.unrealized_usd = internal::mul(out.unrealized, rate);
    } else {
        out.flags |= k_entry_missing;
    }
    out.realized_usd = internal::mul(realized, rate);
    return out;
}

FixedE9 intrinsic(PutCall put_call, FixedE9 strike, FixedE9 price) noexcept {
    const auto gain = put_call == PutCall::Call ? internal::sub(price, strike) : internal::sub(strike, price);
    if (!internal::defined(gain)) {
        return gain;
    }
    return gain.raw > 0 ? gain : FixedE9::from_raw(0);
}

SettledPosition
settle(const Holding& holding, const SettlementPrice& at, UnixNanos settled_at, bool from_last_seen) noexcept {
    SettledPosition out{.holding = holding,
                        .at = at,
                        .settled_at = settled_at,
                        .from_last_seen = from_last_seen,
                        .late = at.by == SettledBy::Index && at.ts.ns > holding.key.expiry.ns + k_settle_late_ns,
                        .units = units_of(holding)};
    if (!internal::positive(at.price)) {
        return out;
    }
    const bool coin = coin_settled(holding);
    const auto rate = coin ? at.price : FixedE9::from_raw(k_e9);
    const auto usd = intrinsic(holding.key.put_call, holding.key.strike, at.price);
    out.payoff = coin ? internal::div(usd, at.price) : usd;
    out.proceeds = internal::mul(out.units, out.payoff);
    out.proceeds_usd = internal::mul(out.proceeds, rate);
    if (internal::positive(holding.entry)) {
        out.realized = internal::mul(out.units, internal::sub(out.payoff, holding.entry));
        out.realized_usd = internal::mul(out.realized, rate);
    }
    return out;
}

Aggregates aggregate(std::span<const PositionRisk> positions,
                     std::span<const SettledPosition> settled,
                     std::span<const BookedRealized> booked,
                     UnixNanos now) {
    std::map<std::string, UnderlyingRisk, std::less<>> underlyings;
    std::map<std::tuple<std::string, std::uint64_t>, ExpiryRisk> expiries;
    Aggregates out{};
    const auto rows = [&](const options::OptionKey& key) {
        auto& u = underlyings[std::string{key.underlying.view()}];
        u.underlying = key.underlying;
        auto& e = expiries[{std::string{key.underlying.view()}, key.expiry.ns}];
        e.underlying = key.underlying;
        e.expiry = key.expiry;
        e.days_to_expiry = internal::days_between(now, key.expiry);
        return std::tuple<Totals&, Totals&>{u.totals, e.totals};
    };
    for (const auto& p : positions) {
        auto [u, e] = rows(p.holding.key);
        internal::add_position(u, p);
        internal::add_position(e, p);
        internal::add_position(out.total, p);
    }
    for (const auto& s : settled) {
        auto [u, e] = rows(s.holding.key);
        for (Totals* totals : {&u, &e, &out.total}) {
            internal::add_realized(*totals, s.realized_usd);
            ++totals->settled;
        }
    }
    for (const auto& b : booked) {
        auto [u, e] = rows(b.key);
        for (Totals* totals : {&u, &e, &out.total}) {
            internal::add_realized(*totals, b.realized_usd);
        }
    }
    for (auto& [name, row] : underlyings) {
        out.by_underlying.push_back(row);
    }
    for (auto& [key, row] : expiries) {
        out.by_expiry.push_back(row);
    }
    return out;
}

FixedE9 revalued(const PositionRisk& position, FixedE9 spot, FixedE9 vol) noexcept {
    const auto& key = position.holding.key;
    const bool coin = coin_settled(position.holding);
    if (!internal::positive(position.vol) || !internal::defined(position.market.mark) ||
        !internal::defined(position.model_price)) {
        return FixedE9::undef();
    }
    const auto forward = internal::add(position.market.forward, internal::mul(position.market.forward, spot));
    const auto shocked = vol.raw == 0 ? position.vol : std::max(internal::add(position.vol, vol), k_min_shocked_vol);
    const auto price = internal::model(coin, forward, key.strike, shocked, position.years, key.put_call).price;
    const auto value = internal::add(position.market.mark, internal::sub(price, position.model_price));
    if (!internal::defined(value)) {
        return value;
    }
    return value.raw > 0 ? value : FixedE9::from_raw(0);
}

ScenarioMatrix
scenario(std::span<const PositionRisk> positions, std::span<const FixedE9> spot, std::span<const FixedE9> vol) {
    ScenarioMatrix out{.spot = {spot.begin(), spot.end()}, .vol = {vol.begin(), vol.end()}};
    out.cells.reserve(spot.size() * vol.size());
    for (const auto v : vol) {
        for (const auto s : spot) {
            out.cells.push_back({.spot = s, .vol = v, .value_usd = FixedE9::from_raw(0)});
        }
    }
    for (const auto& p : positions) {
        const bool coin = coin_settled(p.holding);
        const auto rate = coin ? p.market.rate : FixedE9::from_raw(k_e9);
        if (!internal::defined(p.value_usd) ||
            !internal::defined(revalued(p, FixedE9::from_raw(0), FixedE9::from_raw(0)))) {
            out.excluded.push_back(p.holding.instrument);
            continue;
        }
        ++out.positions;
        out.base_value_usd = internal::add(out.base_value_usd, p.value_usd);
        for (auto& cell : out.cells) {
            // a coin premium's usd value moves with the coin as well as the option
            const auto shocked_rate = coin ? internal::add(rate, internal::mul(rate, cell.spot)) : rate;
            const auto one = internal::mul(internal::mul(p.units, revalued(p, cell.spot, cell.vol)), shocked_rate);
            cell.value_usd = internal::add(cell.value_usd, one);
        }
    }
    for (std::size_t i = 0; i < out.cells.size(); ++i) {
        auto& cell = out.cells[i];
        cell.pnl_usd = internal::sub(cell.value_usd, out.base_value_usd);
        if (internal::defined(cell.pnl_usd) &&
            (!out.worst.has_value() || cell.pnl_usd < out.cells[*out.worst].pnl_usd)) {
            out.worst = i;
        }
    }
    return out;
}

} // namespace raikiri::optionrisk
