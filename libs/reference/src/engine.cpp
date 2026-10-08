#include <raikiri/reference/engine.hpp>

#include <algorithm>
#include <array>
#include <set>

namespace raikiri::reference {

namespace {

[[nodiscard]] std::span<const Quote> quotes_of(const std::map<std::string_view, const AssetQuotes*>& by_asset,
                                               std::string_view asset) {
    const auto found = by_asset.find(asset);
    return found == by_asset.end() ? std::span<const Quote>{} : std::span<const Quote>{found->second->quotes};
}

[[nodiscard]] std::int64_t usd_turnover(const Quote& q, const StableRates& rates) {
    std::int64_t rate = 0;
    switch (q.ccy) {
    case QuoteCcy::Usd:
        rate = k_e9;
        break;
    case QuoteCcy::Usdt:
        rate = rates.usdt_usd.raw;
        break;
    case QuoteCcy::Usdc:
        rate = rates.usdc_usd.raw;
        break;
    case QuoteCcy::Other:
        return 0;
    }
    if (q.volume_24h.is_undef() || q.volume_24h.raw <= 0) {
        return 0;
    }
    return internal::scaled(q.volume_24h.raw, rate, k_e6 * k_e9).value_or(0);
}

} // namespace

bool is_stablecoin(std::string_view asset) noexcept {
    static constexpr std::array<std::string_view, 12> k_stables{"crypto:USDT",
                                                                "crypto:USDC",
                                                                "crypto:FDUSD",
                                                                "crypto:USDE",
                                                                "crypto:DAI",
                                                                "crypto:TUSD",
                                                                "crypto:USD1",
                                                                "crypto:PYUSD",
                                                                "crypto:RLUSD",
                                                                "crypto:USDS",
                                                                "crypto:EUR",
                                                                "crypto:EURC"};
    return std::ranges::find(k_stables, asset) != k_stables.end();
}

std::vector<std::string>
rank_universe(const Universe& spec, std::span<const AssetQuotes> book, const StableRates& rates) {
    std::vector<std::string> out = spec.pinned;
    std::vector<std::pair<int128_t, const std::string*>> ranked;
    for (const auto& entry : book) {
        if (is_stablecoin(entry.asset) || std::ranges::find(out, entry.asset) != out.end()) {
            continue;
        }
        std::set<VenueId> venues;
        std::set<VenueId> spot;
        int128_t turnover = 0;
        for (const auto& q : entry.quotes) {
            venues.insert(q.venue);
            if (q.market == MarketType::Spot) {
                spot.insert(q.venue);
            }
            turnover += usd_turnover(q, rates);
        }
        if (venues.size() >= spec.min_venues && spot.size() >= spec.min_spot_venues && turnover > 0) {
            ranked.emplace_back(turnover, &entry.asset);
        }
    }
    std::ranges::sort(ranked, [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : *a.second < *b.second;
    });
    for (std::size_t i = 0; i < std::min(spec.top_n, ranked.size()); ++i) {
        out.push_back(*ranked[i].second);
    }
    return out;
}

ComputeReferences::ComputeReferences(QuoteSource& source,
                                     ReferencePublisher& sink,
                                     Params params,
                                     Universe universe,
                                     WallClock wall,
                                     std::uint64_t rerank_ns)
    : source_{&source}, sink_{&sink}, params_{params}, universe_{std::move(universe)}, wall_{std::move(wall)},
      rerank_ns_{rerank_ns} {}

CycleReport ComputeReferences::operator()() {
    const auto now = wall_();
    const auto book = source_->snapshot();
    std::map<std::string_view, const AssetQuotes*> by_asset;
    std::size_t quotes = 0;
    for (const auto& entry : book) {
        by_asset.emplace(entry.asset, &entry);
        quotes += entry.quotes.size();
    }

    CycleReport report{.at_ns = now, .quotes = quotes};
    report.rates = stable_rates(quotes_of(by_asset, k_usdt_key), quotes_of(by_asset, k_usdc_key), params_, now);

    if (members_.empty() || now - ranked_at_ >= rerank_ns_) {
        ranked_at_ = now;
        for (auto& asset : rank_universe(universe_, book, report.rates)) {
            if (std::ranges::find(members_, asset) == members_.end()) {
                members_.push_back(std::move(asset));
            }
        }
    }

    std::vector<Row> rows;
    rows.reserve(members_.size());
    for (const auto& asset : members_) {
        auto& state = states_[asset];
        auto ref = compute(quotes_of(by_asset, asset), report.rates, params_, state, now);
        report.refused += ref.refused ? 1 : 0;
        rows.push_back({.asset = asset, .ref = std::move(ref)});
    }
    const auto done = wall_();
    report.took_ns = done > now ? done - now : 0;
    sink_->publish(rows, report);
    return report;
}

} // namespace raikiri::reference
