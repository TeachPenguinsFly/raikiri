#include <raikiri/exec/order_gate.hpp>

#include <algorithm>

namespace raikiri::exec {

void RateLimiter::refill(MonoNanos now) noexcept {
    if (!started_) {
        started_ = true;
        last_refill_ns_ = now.ns;
        return;
    }
    if (now.ns <= last_refill_ns_ || window_ns_ == 0) {
        return;
    }
    const auto elapsed = now.ns - last_refill_ns_;
    // Integer arithmetic throughout: tokens earned is elapsed * capacity /
    // window, and the remainder stays owed by advancing the clock only by
    // what was actually paid for. Rounding it away would leak throughput on
    // every call and eventually earn the ban this class exists to avoid.
    const auto earned = (elapsed * capacity_) / window_ns_;
    if (earned == 0) {
        return;
    }
    tokens_ = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(capacity_, static_cast<std::uint64_t>(tokens_) + earned));
    last_refill_ns_ += (earned * window_ns_) / capacity_;
}

std::expected<void, RateDenied> RateLimiter::take(std::uint32_t cost, MonoNanos now) noexcept {
    refill(now);
    if (now.ns < blocked_until_ns_) {
        ++denials_;
        return std::unexpected{RateDenied{.retry_after_ns = blocked_until_ns_ - now.ns}};
    }
    if (cost > capacity_) {
        // Asking for more than the bucket can ever hold would wait forever;
        // that is a misconfigured cost, not congestion.
        ++denials_;
        return std::unexpected{RateDenied{.retry_after_ns = 0}};
    }
    if (tokens_ < cost) {
        ++denials_;
        const auto missing = cost - tokens_;
        return std::unexpected{RateDenied{
            .retry_after_ns = (static_cast<std::uint64_t>(missing) * window_ns_) / capacity_}};
    }
    tokens_ -= cost;
    return {};
}

std::uint32_t RateLimiter::available(MonoNanos now) noexcept {
    refill(now);
    return now.ns < blocked_until_ns_ ? 0 : tokens_;
}

void RateLimiter::penalize(MonoNanos now, std::uint64_t backoff_ns) noexcept {
    tokens_ = 0;
    started_ = true;
    last_refill_ns_ = now.ns;
    blocked_until_ns_ = std::max(blocked_until_ns_, now.ns + backoff_ns);
}

bool InFlightTable::add(const InFlight& request) noexcept {
    if (entries_.size() >= entries_.capacity()) {
        return false;
    }
    entries_.push_back(request);
    return true;
}

bool InFlightTable::resolve(ClientOrderId id) noexcept {
    const auto it = std::find_if(entries_.begin(), entries_.end(), [id](const InFlight& entry) {
        return entry.client_order_id.raw == id.raw;
    });
    if (it == entries_.end()) {
        return false;
    }
    // Swap-and-pop: order does not matter and expire() scans the whole table
    // anyway, so keeping insertion order would buy nothing.
    *it = entries_.back(); // NOLINT: plain aggregate copy
    entries_.pop_back();
    return true;
}

std::vector<InFlight> InFlightTable::expire(MonoNanos now, std::uint64_t timeout_ns) {
    std::vector<InFlight> out;
    for (std::size_t i = 0; i < entries_.size();) {
        if (now.ns - entries_[i].sent.ns >= timeout_ns) {
            out.push_back(entries_[i]);
            entries_[i] = entries_.back();
            entries_.pop_back();
            ++expired_total_;
            continue;
        }
        ++i;
    }
    return out;
}

} // namespace raikiri::exec
