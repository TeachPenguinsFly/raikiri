#include <raikiri/exec/order_gate.hpp>

#include <algorithm>

namespace raikiri::exec {

void RateLimiter::refill(MonoNanos now) noexcept {
    if (!started_) {
        started_ = true;
        last_refill_ns_ = now.ns;
        window_start_ns_ = now.ns;
        return;
    }
    if (now.ns <= last_refill_ns_ || window_ns_ == 0) {
        return;
    }
    const auto elapsed = now.ns - last_refill_ns_;
    const auto earned = (elapsed * capacity_) / window_ns_;
    if (earned == 0) {
        return;
    }
    tokens_ = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(capacity_, static_cast<std::uint64_t>(tokens_) + earned));
    last_refill_ns_ += (earned * window_ns_) / capacity_;
}

std::expected<void, RateDenied>
RateLimiter::take(std::uint32_t cost, MonoNanos now, RateClass cls) noexcept {
    refill(now);
    if (now.ns < blocked_until_ns_) {
        ++denials_;
        return std::unexpected{RateDenied{.retry_after_ns = blocked_until_ns_ - now.ns}};
    }
    // The floor this class may not spend below. An order may take the bucket
    // to nothing; a poll must leave the reserve standing for one.
    // A reserve wider than the bucket leaves a poll nothing, which is a
    // configuration error rather than an unlimited allowance: clamped here so
    // the subtraction below cannot wrap and hand out everything.
    const std::uint32_t floor = cls == RateClass::Order ? 0U : std::min(reserve_, capacity_);
    if (cost > capacity_ - floor) {
        // Larger than this class can ever afford: waiting cannot help, so the
        // caller is told so rather than handed a retry that will fail again.
        ++denials_;
        return std::unexpected{RateDenied{.retry_after_ns = 0}};
    }
    if (tokens_ < floor + cost) {
        ++denials_;
        const auto missing = floor + cost - tokens_;
        return std::unexpected{RateDenied{
            .retry_after_ns = (static_cast<std::uint64_t>(missing) * window_ns_) / capacity_}};
    }
    // AND THE WINDOW ITSELF, which the bucket does not enforce; see the header.
    if (window_ns_ != 0) {
        if (now.ns - window_start_ns_ >= window_ns_) {
            window_start_ns_ = now.ns;
            window_spent_ = 0;
        }
        if (static_cast<std::uint64_t>(window_spent_) + cost > capacity_) {
            ++denials_;
            return std::unexpected{
                RateDenied{.retry_after_ns = window_ns_ - (now.ns - window_start_ns_)}};
        }
        window_spent_ += cost;
    }
    tokens_ -= cost;
    return {};
}

std::uint32_t RateLimiter::available(MonoNanos now, RateClass cls) noexcept {
    refill(now);
    if (now.ns < blocked_until_ns_) {
        return 0;
    }
    const std::uint32_t floor = cls == RateClass::Order ? 0U : std::min(reserve_, capacity_);
    return tokens_ > floor ? tokens_ - floor : 0;
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
