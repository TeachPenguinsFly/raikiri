#include <raikiri/optiondata/queries.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>

namespace raikiri::optiondata {

namespace internal {

[[nodiscard]] const UnderlyingListing* find(const std::vector<UnderlyingListing>& all, std::string_view name) {
    const auto found = std::ranges::find_if(all,
                                            [name](const UnderlyingListing& u) { return u.underlying.view() == name; });
    return found == all.end() ? nullptr : &*found;
}

[[nodiscard]] std::string cache_key(std::string_view underlying, std::span<const VenueId> venues) {
    std::string key{underlying};
    for (const auto venue : venues) {
        key += '|';
        key += to_string(venue);
    }
    return key;
}

} // namespace internal

std::optional<UnixNanos> resolve_expiry(const UnderlyingListing& listing, std::string_view expiry) noexcept {
    if (expiry.size() == 8) {
        for (const auto& e : listing.expiries) {
            if (expiry_date(e.expiry) == expiry) {
                return e.expiry;
            }
        }
        return std::nullopt;
    }
    std::uint64_t ns = 0;
    const auto [end, ec] = std::from_chars(expiry.data(), expiry.data() + expiry.size(), ns);
    if (ec != std::errc{} || end != expiry.data() + expiry.size()) {
        return std::nullopt;
    }
    for (const auto& e : listing.expiries) {
        if (e.expiry.ns == ns) {
            return e.expiry;
        }
    }
    return std::nullopt;
}

std::expected<ChainView, ChainError> GetChain::operator()(const ChainRequest& request) const {
    const auto all = market_->underlyings();
    const auto* listing = internal::find(all, request.underlying);
    if (listing == nullptr) {
        return std::unexpected(ChainError::NoUnderlying);
    }
    const auto expiry = resolve_expiry(*listing, request.expiry);
    if (!expiry.has_value()) {
        return std::unexpected(ChainError::NoExpiry);
    }
    market_->read(request.underlying, expiry, quotes_);
    auto chain = build_chain(quotes_, *expiry, now_(), request.venues);
    chain.underlying = listing->underlying;
    return chain;
}

std::optional<SurfaceView> GetSurface::operator()(std::string_view underlying, std::span<const VenueId> venues) {
    const auto now = now_();
    auto& entry = cache_[internal::cache_key(underlying, venues)];
    if (entry.pending.valid() && entry.pending.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
        entry.view = entry.pending.get();
    }
    const bool fresh = entry.view.has_value() && now.ns >= entry.view->as_of.ns &&
                       now.ns - entry.view->as_of.ns < refresh_ns_;
    if (fresh || entry.pending.valid()) {
        return entry.view;
    }
    const auto all = market_->underlyings();
    const auto* listing = internal::find(all, underlying);
    if (listing == nullptr) {
        entry.view.reset();
        return std::nullopt;
    }
    std::vector<OptionQuote> quotes;
    market_->read(underlying, std::nullopt, quotes);
    if (!venues.empty()) {
        std::erase_if(quotes,
                      [venues](const OptionQuote& q) { return !std::ranges::contains(venues, q.instrument.venue()); });
    }
    auto fit = [quotes = std::move(quotes), now, name = listing->underlying] {
        auto view = build_surface(quotes, now);
        view.underlying = name;
        return view;
    };
    if (refit_ == Refit::Background && entry.view.has_value()) {
        entry.pending = std::async(std::launch::async, std::move(fit));
    } else {
        entry.view = fit();
    }
    return entry.view;
}

void GetSurface::settle() {
    for (auto& [key, entry] : cache_) {
        if (entry.pending.valid()) {
            entry.view = entry.pending.get();
        }
    }
}

} // namespace raikiri::optiondata
