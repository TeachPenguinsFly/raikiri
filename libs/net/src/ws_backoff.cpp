#include <raikiri/net/ws_backoff.hpp>

#include <algorithm>
#include <charconv>
#include <map>

namespace raikiri::net {

namespace internal {

// a venue asking for longer than this is clamped, so a typo cannot park a host for days
constexpr std::uint64_t k_retry_after_ceiling_ns = 3'600'000'000'000ULL;

// between asks while another socket's handshake decides for the host
constexpr std::uint64_t k_probe_poll_ns = 1'000'000'000ULL;

std::mutex g_hosts_mu;
std::map<std::string, std::shared_ptr<HostBackoff>, std::less<>> g_hosts;

} // namespace internal

HostBackoff::HostBackoff(WsBackoffPolicy policy, std::uint64_t seed)
    : policy_{policy}, attempts_{policy.attempts, policy.attempts_window_ns},
      rng_{static_cast<std::minstd_rand::result_type>(seed | 1U)} {}

std::uint64_t HostBackoff::admit(std::uint64_t now_ns) noexcept {
    const std::scoped_lock lock{mu_};
    if (now_ns < until_ns_) {
        return until_ns_ - now_ns;
    }
    const bool probing = streak_ != 0 && !proven_;
    if (probing && now_ns < probe_until_ns_) {
        return std::min(probe_until_ns_ - now_ns, internal::k_probe_poll_ns);
    }
    if (const auto wait = attempts_.wait_ns(1, now_ns); wait != 0) {
        return wait;
    }
    attempts_.spend(1, now_ns);
    if (probing) {
        probe_until_ns_ = now_ns + policy_.probe_timeout_ns;
    }
    return 0;
}

void HostBackoff::opened() noexcept {
    const std::scoped_lock lock{mu_};
    probe_until_ns_ = 0;
    proven_ = true;
}

std::uint64_t HostBackoff::failed(std::uint64_t now_ns, WsFailure kind, std::uint64_t retry_after_ns) noexcept {
    const std::scoped_lock lock{mu_};
    probe_until_ns_ = 0;
    proven_ = false;
    if (kind == WsFailure::Refused) {
        ++refusals_;
    }
    const bool escalates = kind == WsFailure::Refused && kind_ == WsFailure::Transport;
    if (now_ns < until_ns_ && !escalates && retry_after_ns <= until_ns_ - now_ns) {
        return 0;
    }
    if (escalates) {
        kind_ = WsFailure::Refused;
        streak_ = 0;
    }
    const bool refused = kind_ == WsFailure::Refused;
    const auto floor = refused ? policy_.refused_floor_ns : policy_.transport_floor_ns;
    const auto cap = refused ? policy_.refused_cap_ns : policy_.transport_cap_ns;
    auto base = floor;
    for (std::uint32_t i = 0; i < streak_ && base < cap; ++i) {
        base *= 2;
    }
    base = std::min(base, cap);
    ++streak_;
    const auto spread = base / 4;
    auto delay = base - spread + (spread == 0 ? 0 : rng_() % (spread + 1));
    delay = std::max(delay, std::min(retry_after_ns, internal::k_retry_after_ceiling_ns));
    until_ns_ = std::max(until_ns_, now_ns + delay);
    ++backoffs_;
    return delay;
}

void HostBackoff::abandoned() noexcept {
    const std::scoped_lock lock{mu_};
    probe_until_ns_ = 0;
}

void HostBackoff::healthy() noexcept {
    const std::scoped_lock lock{mu_};
    streak_ = 0;
    kind_ = WsFailure::Transport;
    probe_until_ns_ = 0;
}

WsBackoffStats HostBackoff::stats() const noexcept {
    const std::scoped_lock lock{mu_};
    return {.backoffs = backoffs_, .refusals = refusals_, .streak = streak_, .until_ns = until_ns_};
}

std::shared_ptr<HostBackoff> host_backoff(std::string_view host, std::string_view port, const WsBackoffPolicy& policy) {
    std::string key{host};
    key += ':';
    key += port;
    const std::scoped_lock lock{internal::g_hosts_mu};
    auto& slot = internal::g_hosts[key];
    if (!slot) {
        slot = std::make_shared<HostBackoff>(policy, std::hash<std::string>{}(key));
    }
    return slot;
}

std::uint64_t retry_after_ns(std::string_view header) noexcept {
    std::uint64_t seconds = 0;
    const auto [stop, ec] = std::from_chars(header.data(), header.data() + header.size(), seconds);
    if (ec != std::errc{} || stop != header.data() + header.size() || seconds > 86'400) {
        return 0;
    }
    return seconds * 1'000'000'000ULL;
}

} // namespace raikiri::net
