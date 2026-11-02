#include <raikiri/optionrisk/queries.hpp>

#include <algorithm>
#include <set>

namespace raikiri::optionrisk {

namespace internal {

[[nodiscard]] bool above_zero(FixedE9 value) noexcept {
    return !value.is_undef() && value.raw > 0;
}

[[nodiscard]] FixedE9 median(std::vector<FixedE9>& values) {
    if (values.empty()) {
        return FixedE9::undef();
    }
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::ranges::nth_element(values, middle);
    return *middle;
}

// the coin rate of each venue over the underlying's quotes, for a listing whose own greeks carry none
[[nodiscard]] std::map<VenueId, FixedE9> venue_rates(std::span<const optiondata::OptionQuote> quotes) {
    std::map<VenueId, std::vector<FixedE9>> figures;
    for (const auto& q : quotes) {
        if (q.greeks.has_value() && above_zero(q.greeks->index_price)) {
            figures[q.instrument.venue()].push_back(q.greeks->index_price);
        }
    }
    std::map<VenueId, FixedE9> out;
    for (auto& [venue, values] : figures) {
        out.emplace(venue, median(values));
    }
    return out;
}

} // namespace internal

std::optional<SettlementPrice>
IndexSettlements::price(VenueId venue, std::string_view underlying, UnixNanos expiry) const {
    market_->read(underlying, std::nullopt, quotes_);
    std::optional<SettlementPrice> out;
    for (const auto& q : quotes_) {
        if (q.instrument.venue() != venue || !q.greeks.has_value() || !internal::above_zero(q.greeks->index_price)) {
            continue;
        }
        const auto ts = q.greeks->ts_event;
        if (ts.ns >= expiry.ns && (!out.has_value() || ts.ns < out->ts.ns)) {
            out = SettlementPrice{.price = q.greeks->index_price, .by = SettledBy::Index, .ts = ts};
        }
    }
    return out;
}

OptionRisk::OptionRisk(const Sources& sources, optiondata::WallClock now, Staleness staleness)
    : sources_{sources}, now_{std::move(now)}, staleness_{staleness}, settled_{sources.ledger->load()} {}

bool OptionRisk::booked(const Holding& holding) const {
    return std::ranges::any_of(settled_, [&](const SettledPosition& s) {
        return s.holding.account == holding.account && s.holding.instrument == holding.instrument &&
               s.holding.key.expiry == holding.key.expiry;
    });
}

void OptionRisk::sweep() {
    sweep_at(now_());
}

void OptionRisk::sweep_at(UnixNanos now) {
    sources_.holdings->read(read_);
    std::set<Key> reported;
    for (const auto& holding : read_) {
        const Key key{holding.account, holding.instrument.packed};
        reported.insert(key);
        if (!booked(holding)) {
            open_[key] = holding;
        }
    }
    awaiting_.clear();
    for (auto it = open_.begin(); it != open_.end();) {
        const auto& holding = it->second;
        const bool seen = reported.contains(it->first);
        if (now.ns < holding.key.expiry.ns) {
            // a detached gateway says nothing, so its last report stands, flagged stale by its age
            const bool closed = !seen && sources_.holdings->attached(holding.account);
            it = closed ? open_.erase(it) : std::next(it);
            continue;
        }
        const auto price = sources_.settlements->price(
            holding.instrument.venue(), holding.key.underlying.view(), holding.key.expiry);
        if (!price.has_value()) {
            awaiting_.push_back(holding);
            ++it;
            continue;
        }
        const auto done = settle(holding, *price, now, !seen);
        sources_.ledger->append(done);
        settled_.push_back(done);
        it = open_.erase(it);
    }
}

void OptionRisk::refresh_realized(UnixNanos now) {
    if (sources_.realized == nullptr || now.ns < realized_due_) {
        return;
    }
    realized_due_ = now.ns + k_realized_refresh_ns;
    sources_.realized->read(realized_);
    realized_by_id_.clear();
    for (const auto& row : realized_) {
        realized_by_id_[row.instrument.packed] = row.realized;
    }
}

void OptionRisk::assess_underlying(std::string_view underlying, UnixNanos now, std::vector<PositionRisk>& out) {
    sources_.market->read(underlying, std::nullopt, quotes_);
    std::map<std::uint64_t, std::vector<optiondata::OptionQuote>> held;
    for (const auto& [key, holding] : open_) {
        if (holding.key.underlying.view() == underlying) {
            held[holding.key.expiry.ns];
        }
    }
    std::unordered_map<std::uint64_t, const optiondata::OptionQuote*> by_id;
    by_id.reserve(quotes_.size());
    for (const auto& q : quotes_) {
        by_id[q.instrument.packed] = &q;
        if (const auto expiry = held.find(q.key.expiry.ns); expiry != held.end()) {
            expiry->second.push_back(q);
        }
    }
    const auto rates = internal::venue_rates(quotes_);
    std::map<std::uint64_t, FixedE9> forwards;
    for (const auto& [key, holding] : open_) {
        if (holding.key.underlying.view() != underlying || now.ns >= holding.key.expiry.ns) {
            continue;
        }
        auto forward = forwards.find(holding.key.expiry.ns);
        if (forward == forwards.end()) {
            forward = forwards
                          .emplace(holding.key.expiry.ns, optiondata::expiry_forward(held[holding.key.expiry.ns]).value)
                          .first;
        }
        const auto rate = rates.find(holding.instrument.venue());
        const auto fallback = rate != rates.end() ? rate->second : forward->second;
        MarketState market{.forward = forward->second, .rate = fallback};
        if (const auto quote = by_id.find(holding.instrument.packed); quote != by_id.end()) {
            market = market_of(*quote->second, forward->second, fallback);
        }
        const auto realized = realized_by_id_.find(holding.instrument.packed);
        out.push_back(assess(
            holding, market, realized != realized_by_id_.end() ? realized->second : FixedE9::undef(), now, staleness_));
    }
}

RiskView OptionRisk::view() {
    const auto now = now_();
    sweep_at(now);
    refresh_realized(now);

    std::set<std::string, std::less<>> underlyings;
    for (const auto& [key, holding] : open_) {
        underlyings.emplace(holding.key.underlying.view());
    }
    RiskView out{.as_of = now,
                 .awaiting_settlement = awaiting_,
                 .settled = settled_,
                 .realized_known = sources_.realized != nullptr};
    out.positions.reserve(open_.size());
    for (const auto& underlying : underlyings) {
        assess_underlying(underlying, now, out.positions);
    }

    std::vector<BookedRealized> booked;
    booked.reserve(realized_.size());
    std::map<std::string, std::map<VenueId, FixedE9>, std::less<>> rates;
    for (const auto& row : realized_) {
        BookedRealized one{.key = row.key};
        if (row.premium == OptionUnit::Quote) {
            one.realized_usd = row.realized;
        } else if (row.premium == OptionUnit::Base) {
            const auto name = std::string{row.key.underlying.view()};
            auto found = rates.find(name);
            if (found == rates.end()) {
                sources_.market->read(name, std::nullopt, quotes_);
                found = rates.emplace(name, internal::venue_rates(quotes_)).first;
            }
            if (const auto rate = found->second.find(row.instrument.venue()); rate != found->second.end()) {
                one.realized_usd = times(row.realized, rate->second);
            }
        }
        booked.push_back(one);
    }
    out.aggregates = aggregate(out.positions, settled_, booked, now);
    return out;
}

ScenarioMatrix
OptionRisk::scenarios(std::string_view underlying, std::span<const FixedE9> spot, std::span<const FixedE9> vol) {
    const auto now = now_();
    sweep_at(now);
    refresh_realized(now);
    std::vector<PositionRisk> positions;
    assess_underlying(underlying, now, positions);
    return scenario(positions, spot, vol);
}

} // namespace raikiri::optionrisk
