#pragma once

// a venue's socket limits from config/venues.yaml, set once per process

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
    std::size_t subscriptions_per_frame{0};
    WsRate socket_messages;
    std::vector<WsRate> ip_messages;
    WsRate connects;
};

// the venue's ws rows for `market`; RAIKIRI_<VENUE>_WS_SHARE scales the per-ip ones
[[nodiscard]] WsLimits
ws_limits(std::string_view venue, const config::VenueSettings& settings, std::string_view market);

// a thread-safe sliding log
class Meter {
public:
    explicit Meter(WsRate rate) : log_{rate.max, rate.window_ns} {}
    // zero and spent, or how long until one may be
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
    // one outbound frame, against every per-ip message limit
    [[nodiscard]] bool take_message(std::uint64_t now_ns) noexcept;
    // the venue refused for pace: nothing more for `hold_ns`
    void hold_messages(std::uint64_t now_ns, std::uint64_t hold_ns) noexcept;
    // zero and spent, or how long until a socket may connect
    [[nodiscard]] std::uint64_t connect(std::uint64_t now_ns) noexcept;
    // how many of `wanted` subscriptions the ip still has room for; those are claimed
    [[nodiscard]] std::size_t claim_channels(std::size_t wanted) noexcept;

private:
    std::vector<std::unique_ptr<Meter>> messages_;
    std::unique_ptr<Meter> connects_;
    mutable std::mutex mu_;
    std::size_t channels_max_{0};
    std::size_t channels_used_{0};
};

// subscriptions per subscribe frame; one when unconfigured
[[nodiscard]] std::size_t frame_subscriptions() noexcept;

void set_process_ws_limits(const WsLimits& limits);
[[nodiscard]] const WsLimits& process_ws_limits() noexcept;
[[nodiscard]] IpMeters& process_ip_meters() noexcept;
[[nodiscard]] std::uint64_t steady_ns() noexcept;

} // namespace raikiri::net
