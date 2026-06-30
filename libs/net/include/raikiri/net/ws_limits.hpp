#pragma once

// A venue's socket limits, from the ws_* rows of its venues.yaml table. A
// gateway runs one venue, so it sets them once for the process and every
// WsPool built afterwards takes them; the per-IP meters are process-wide and
// therefore survive every reconnect.

#include <raikiri/common/sliding_log.hpp>
#include <raikiri/config/settings.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace raikiri::net {

struct WsRate {
    std::uint32_t max{0};
    std::uint64_t window_ns{0};
};

struct WsLimits {
    bool configured{false};
    std::size_t streams_per_socket{0};
    std::size_t sockets{0};
    std::size_t channels{0};
    WsRate socket_messages;
    std::vector<WsRate> ip_messages;
    WsRate connects;
};

// The venue's ws rows for `market` ("perp" or "spot"; a row naming no market
// applies to both). RAIKIRI_<VENUE>_WS_SHARE, a percentage, scales the per-IP
// ones for a process that shares its IP with others of the same venue.
[[nodiscard]] WsLimits
ws_limits(std::string_view venue, const config::VenueSettings& settings, std::string_view market);

// A thread-safe sliding log.
class Meter {
public:
    explicit Meter(WsRate rate) : log_{rate.max, rate.window_ns} {}
    // Zero and spent, or how long until one may be.
    [[nodiscard]] std::uint64_t take(std::uint64_t now_ns) noexcept;
    [[nodiscard]] std::uint64_t wait(std::uint64_t now_ns) noexcept;
    void hold(std::uint64_t now_ns, std::uint64_t hold_ns) noexcept;

private:
    std::mutex mu_;
    SlidingLog log_;
};

class IpMeters {
public:
    explicit IpMeters(const WsLimits& limits);
    // One outbound frame, against every per-IP message limit.
    [[nodiscard]] bool take_message(std::uint64_t now_ns) noexcept;
    // The venue refused for pace: nothing more for `hold_ns`.
    void hold_messages(std::uint64_t now_ns, std::uint64_t hold_ns) noexcept;
    // Zero and spent, or how long until a socket may connect.
    [[nodiscard]] std::uint64_t connect(std::uint64_t now_ns) noexcept;
    // How many of `wanted` subscriptions the IP still has room for; those are claimed.
    [[nodiscard]] std::size_t claim_channels(std::size_t wanted) noexcept;
    [[nodiscard]] std::size_t channels_claimed() const noexcept;

private:
    std::vector<std::unique_ptr<Meter>> messages_;
    std::unique_ptr<Meter> connects_;
    mutable std::mutex mu_;
    std::size_t channels_max_{0};
    std::size_t channels_used_{0};
};

void set_process_ws_limits(const WsLimits& limits);
[[nodiscard]] const WsLimits& process_ws_limits() noexcept;
[[nodiscard]] IpMeters& process_ip_meters() noexcept;
[[nodiscard]] std::uint64_t steady_ns() noexcept;

} // namespace raikiri::net
