#include <raikiri/clientapi/domain/guards.hpp>

#include <algorithm>
#include <charconv>

namespace raikiri::clientapi {

namespace {

constexpr std::uint64_t k_nanos = 1'000'000'000ULL;

} // namespace

std::string canonical_request(std::string_view timestamp_ms,
                              std::string_view method,
                              std::string_view path,
                              std::string_view query,
                              std::string_view body) {
    std::string out;
    out.reserve(timestamp_ms.size() + method.size() + path.size() + query.size() + body.size() + 4);
    out += timestamp_ms;
    out += '\n';
    out += method;
    out += '\n';
    out += path;
    out += '\n';
    out += query;
    out += '\n';
    out += body;
    return out;
}

std::optional<std::uint64_t> parse_millis(std::string_view text) noexcept {
    std::uint64_t value = 0;
    const auto [stop, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || stop != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

TokenBucket::TokenBucket(std::uint32_t per_second, std::uint32_t burst, std::uint64_t now_ns) noexcept
    : per_second_{std::max<std::uint64_t>(per_second, 1)},
      capacity_{std::uint64_t{std::max<std::uint32_t>(burst, 1)} * k_nanos}, tokens_{capacity_}, last_ns_{now_ns},
      burst_{std::max<std::uint32_t>(burst, 1)} {}

BucketView TokenBucket::take(std::uint64_t now_ns) noexcept {
    if (now_ns > last_ns_) {
        // past a full refill the product is not needed, and on a long idle it would overflow
        const auto missing = capacity_ - tokens_;
        const auto elapsed = now_ns - last_ns_;
        tokens_ = elapsed >= missing / per_second_ + 1 ? capacity_ : tokens_ + elapsed * per_second_;
        last_ns_ = now_ns;
    }
    const auto ceil_seconds = [this](std::uint64_t nano_tokens) {
        return static_cast<std::uint32_t>((nano_tokens + per_second_ * k_nanos - 1) / (per_second_ * k_nanos));
    };
    BucketView view{.limit = burst_};
    if (tokens_ >= k_nanos) {
        tokens_ -= k_nanos;
        view.allowed = true;
    } else {
        view.retry_after_s = ceil_seconds(k_nanos - tokens_);
    }
    view.remaining = static_cast<std::uint32_t>(tokens_ / k_nanos);
    view.reset_s = ceil_seconds(capacity_ - tokens_);
    return view;
}

bool ReplayGuard::admit(std::string_view signature, std::uint64_t now_ms) {
    while (!order_.empty() && order_.front().expires_ms <= now_ms) {
        seen_.erase(order_.front().signature);
        order_.pop_front();
    }
    auto [at, fresh] = seen_.emplace(signature);
    if (!fresh) {
        return false;
    }
    order_.push_back({.signature = *at, .expires_ms = now_ms + window_ms_});
    return true;
}

IdemBegin IdempotencyTable::begin(const std::string& key, std::string_view request_hash, std::uint64_t now_ns) {
    expire(now_ns);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        entries_.emplace(key, IdemEntry{.request_hash = std::string{request_hash}, .at_ns = now_ns});
        order_.emplace_back(key, now_ns);
        return {};
    }
    const auto& entry = found->second;
    if (entry.request_hash != request_hash) {
        return {.verdict = IdemVerdict::Mismatch};
    }
    switch (entry.state) {
    case IdemState::Pending:
        return {.verdict = IdemVerdict::InFlight};
    case IdemState::Unknown:
        return {.verdict = IdemVerdict::OutcomeUnknown};
    case IdemState::Done:
        break;
    }
    return {.verdict = IdemVerdict::Replay, .reply = entry.reply};
}

void IdempotencyTable::finish(const std::string& key, StoredReply reply) {
    if (const auto found = entries_.find(key); found != entries_.end()) {
        found->second.state = IdemState::Done;
        found->second.reply = std::move(reply);
    }
}

void IdempotencyTable::mark_unknown(const std::string& key) {
    if (const auto found = entries_.find(key); found != entries_.end()) {
        found->second.state = IdemState::Unknown;
    }
}

void IdempotencyTable::release(const std::string& key) {
    entries_.erase(key);
}

void IdempotencyTable::restore(const std::string& key, IdemEntry entry, std::uint64_t now_ns) {
    if (entry.at_ns + ttl_ns_ <= now_ns) {
        return;
    }
    if (entry.state == IdemState::Pending) {
        entry.state = IdemState::Unknown;
    }
    const auto at = entry.at_ns;
    const auto [slot, fresh] = entries_.insert_or_assign(key, std::move(entry));
    if (fresh) {
        order_.emplace_back(slot->first, at);
    }
}

const IdemEntry* IdempotencyTable::find(const std::string& key) const {
    const auto found = entries_.find(key);
    return found == entries_.end() ? nullptr : &found->second;
}

void IdempotencyTable::expire(std::uint64_t now_ns) {
    while (!order_.empty() && order_.front().second + ttl_ns_ <= now_ns) {
        const auto found = entries_.find(order_.front().first);
        if (found != entries_.end() && found->second.at_ns == order_.front().second) {
            entries_.erase(found);
        }
        order_.pop_front();
    }
}

} // namespace raikiri::clientapi
