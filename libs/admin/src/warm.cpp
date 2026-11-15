#include <raikiri/admin/warm.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <glaze/glaze.hpp>
#include <iterator>
#include <limits>

namespace raikiri::admin {

namespace {

constexpr std::uint64_t k_ms = 1'000'000ULL;

bool contains_fold(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty()) {
        return true;
    }
    const auto up = [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); };
    for (std::size_t start = 0; start + needle.size() <= haystack.size(); ++start) {
        std::size_t i = 0;
        while (i < needle.size() && up(haystack[start + i]) == up(needle[i])) {
            ++i;
        }
        if (i == needle.size()) {
            return true;
        }
    }
    return false;
}

} // namespace

void Jobs::add(std::string name, std::chrono::milliseconds period, std::function<void()> run) {
    jobs_.push_back({.name = std::move(name),
                     .period_ns = static_cast<std::uint64_t>(period.count()) * k_ms,
                     .run = std::move(run)});
}

void Jobs::run(Job& job, MonoNanos now) {
    const auto started = std::chrono::steady_clock::now();
    job.run();
    job.last_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    job.last_ns = now.ns;
    job.due_ns = now.ns + job.period_ns;
    ++job.runs;
}

MonoNanos Jobs::tick(MonoNanos now) {
    auto next = std::numeric_limits<std::uint64_t>::max();
    for (auto& job : jobs_) {
        if (now.ns >= job.due_ns) {
            run(job, now);
        }
        next = std::min(next, job.due_ns);
    }
    return MonoNanos{next};
}

void Jobs::run_now(std::string_view name, MonoNanos now) {
    for (auto& job : jobs_) {
        if (job.name == name) {
            run(job, now);
        }
    }
}

std::vector<Jobs::Status> Jobs::status(MonoNanos now) const {
    std::vector<Status> out;
    out.reserve(jobs_.size());
    for (const auto& job : jobs_) {
        out.push_back({.name = job.name,
                       .period_ms = job.period_ns / k_ms,
                       .runs = job.runs,
                       .last_ms = job.last_ms,
                       .age_ms = job.runs == 0 ? -1 : static_cast<std::int64_t>((now.ns - job.last_ns) / k_ms)});
    }
    return out;
}

std::optional<http::Response> Jobs::route(const http::Request& req, MonoNanos now) const {
    if (req.path != "/cache/jobs") {
        return std::nullopt;
    }
    std::string body;
    if (const auto ec = glz::write_json(status(now), body); ec) {
        return http::problem(500, "cannot encode", "jobs");
    }
    http::Response out;
    out.status = 200;
    out.content_type = "application/json";
    out.body = std::move(body);
    return out;
}

namespace {

// a ticker is not an asset: hyperliquid's spot tokens and gate's long tail reuse names of unrelated coins, and a dead
// book drifts. A listing priced more than this far from the market's busiest listing is its own market
constexpr double k_apart = 0.2;

} // namespace

void split_strangers(std::vector<Market>& markets) {
    std::vector<Market> strays;
    for (auto& market : markets) {
        const Listing* anchor = nullptr;
        for (const auto& l : market.listings) {
            if (l.mid > 0 && l.market != "option" && (anchor == nullptr || l.volume_24h_usd > anchor->volume_24h_usd)) {
                anchor = &l;
            }
        }
        if (anchor == nullptr) {
            continue;
        }
        // three prices or more: their median, which one venue's own volume cannot sway; fewer: the busiest one's
        std::vector<double> mids;
        for (const auto& l : market.listings) {
            if (l.mid > 0 && l.market != "option") {
                mids.push_back(l.mid);
            }
        }
        double ref = anchor->mid;
        if (mids.size() >= 3) {
            std::ranges::nth_element(mids, mids.begin() + static_cast<std::ptrdiff_t>(mids.size() / 2));
            ref = mids[mids.size() / 2];
        }
        std::vector<Listing> kept;
        for (auto& l : market.listings) {
            const bool apart = l.mid > 0 && l.market != "option" && std::abs(l.mid / ref - 1.0) > k_apart;
            if (!apart) {
                kept.push_back(std::move(l));
                continue;
            }
            // one market per venue that disagrees, keyed so the terminal never pairs it with the rest
            const auto key = market.key + "@" + l.venue;
            auto it = std::ranges::find(strays, key, &Market::key);
            if (it == strays.end()) {
                strays.push_back({.key = key, .symbol = l.local_symbol, .listings = {}, .volume_24h_usd = 0.0});
                it = std::prev(strays.end());
            }
            it->volume_24h_usd += l.volume_24h_usd;
            it->listings.push_back(std::move(l));
        }
        // every listing was moved out above, so the kept ones always go back
        const bool split = kept.size() != market.listings.size();
        market.listings = std::move(kept);
        if (split) {
            market.volume_24h_usd = 0.0;
            for (const auto& l : market.listings) {
                market.volume_24h_usd += l.volume_24h_usd;
            }
        }
    }
    for (auto& stray : strays) {
        markets.push_back(std::move(stray));
    }
}

void MarketsCache::refresh() {
    all_ = registry_->markets({}, std::numeric_limits<std::size_t>::max());
    split_strangers(all_);
    Registry::rank(all_);
    warm_ = true;
}

std::vector<Market> MarketsCache::query(std::string_view text, std::size_t limit) const {
    if (!warm_) {
        return registry_->markets(text, limit);
    }
    std::vector<Market> out;
    for (const auto& market : all_) {
        if (out.size() >= limit) {
            break;
        }
        const bool hit = contains_fold(market.key, text) || std::ranges::any_of(market.listings, [&](const Listing& l) {
                             return contains_fold(l.local_symbol, text);
                         });
        if (hit) {
            out.push_back(market);
        }
    }
    return out;
}

} // namespace raikiri::admin
