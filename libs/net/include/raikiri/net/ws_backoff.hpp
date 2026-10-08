#pragma once

// one reconnect budget per venue host, shared by every socket to it, so a refusing venue is not hammered

#include <raikiri/common/sliding_log.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>

namespace raikiri::net {

enum class WsFailure : std::uint8_t {
    Transport,
    Refused, // the handshake answered 429 or 418
};

struct WsBackoffPolicy {
    std::uint64_t transport_floor_ns{1'000'000'000ULL};
    std::uint64_t transport_cap_ns{30'000'000'000ULL};
    std::uint64_t refused_floor_ns{30'000'000'000ULL};
    std::uint64_t refused_cap_ns{600'000'000'000ULL};
    // a socket open this long with data clears the failure streak
    std::uint64_t healthy_ns{60'000'000'000ULL};
    // handshakes per window on one host, whatever each socket's backoff allows
    std::uint32_t attempts{60};
    std::uint64_t attempts_window_ns{60'000'000'000ULL};
    // while failing, one handshake at a time until one opens; an unanswered one frees the slot after this
    std::uint64_t probe_timeout_ns{30'000'000'000ULL};
};

struct WsBackoffStats {
    std::uint64_t backoffs{0};
    std::uint64_t refusals{0};
    std::uint64_t handshakes{0};
    std::uint32_t streak{0};
    std::uint64_t until_ns{0};
};

class HostBackoff {
public:
    explicit HostBackoff(WsBackoffPolicy policy = {}, std::uint64_t seed = 1);

    // zero and the handshake is counted, or how long to wait before asking again
    [[nodiscard]] std::uint64_t admit(std::uint64_t now_ns) noexcept;

    void opened() noexcept;

    // the backoff this failure engaged, or zero when one was already running
    std::uint64_t failed(std::uint64_t now_ns, WsFailure kind, std::uint64_t retry_after_ns = 0) noexcept;

    // an admitted handshake was stopped before it finished
    void abandoned() noexcept;

    void healthy() noexcept;

    [[nodiscard]] const WsBackoffPolicy& policy() const noexcept { return policy_; }

    [[nodiscard]] WsBackoffStats stats() const noexcept;

private:
    WsBackoffPolicy policy_;
    mutable std::mutex mu_;
    SlidingLog attempts_;
    std::minstd_rand rng_;
    WsFailure kind_{WsFailure::Transport};
    std::uint32_t streak_{0};
    std::uint64_t until_ns_{0};
    std::uint64_t probe_until_ns_{0};
    bool proven_{false};
    std::uint64_t backoffs_{0};
    std::uint64_t refusals_{0};
    std::uint64_t handshakes_{0};
};

// the one shared by every socket to host:port; the first caller's policy holds
[[nodiscard]] std::shared_ptr<HostBackoff>
host_backoff(std::string_view host, std::string_view port, const WsBackoffPolicy& policy);

// seconds from a Retry-After header in nanoseconds; zero when absent or a date
[[nodiscard]] std::uint64_t retry_after_ns(std::string_view header) noexcept;

} // namespace raikiri::net
