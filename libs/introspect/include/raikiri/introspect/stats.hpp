#pragma once

// what one gateway reports about itself and what a machine adds up to; pure values

#include <raikiri/introspect/latency.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::introspect {

inline constexpr std::uint32_t k_stats_version = 1;

struct Tally {
    std::string name;
    std::uint64_t count{0};
};

struct CatalogueStats {
    std::uint64_t instruments{0};
    std::vector<Tally> by_market;
    std::vector<Tally> by_asset_class;
    std::vector<Tally> by_kind;
};

struct SubscriptionStats {
    std::string channel;
    std::uint64_t requested{0};
    std::uint64_t accepted{0};
    std::uint64_t refused{0};
    std::uint64_t messages{0};
    double per_second{0};
};

struct SocketStats {
    std::string host;
    std::uint64_t sockets{0};
    std::uint64_t open{0};
    std::uint64_t keys{0};
    std::uint64_t min_keys{0};
    double avg_keys{0};
    std::uint64_t max_keys{0};
    std::uint64_t pending_frames{0};
    std::uint64_t reopens{0};
    std::string last_close_reason;
    std::uint64_t last_close_ns{0};
    std::uint64_t backoffs{0};
    std::uint64_t refusals{0};
    std::uint64_t backoff_ms{0};
};

struct SocketLimits {
    bool configured{false};
    std::uint64_t ws_streams{0};
    std::uint64_t ws_sockets{0};
    std::uint64_t ws_channels{0};
    std::uint64_t ws_subscriptions_per_frame{0};
    std::uint64_t ws_socket_messages{0};
    std::uint64_t ws_socket_window_ms{0};
    std::uint64_t ws_connects{0};
    std::uint64_t ws_connects_window_ms{0};
};

struct CoverageStats {
    std::uint64_t subscribed{0};
    std::uint64_t priced_60s{0};
    std::uint64_t priced_ever{0};
    std::uint64_t books_seeded{0};
    std::uint64_t books_unseeded{0};
    std::uint64_t funding_received{0};
};

struct RestLimitStats {
    std::string name;
    std::uint64_t max{0};
    std::uint64_t window_ms{0};
    std::uint64_t calls{0};
    std::uint64_t denied{0};
};

struct RestEndpointStats {
    std::string endpoint;
    std::uint64_t calls{0};
    std::uint64_t refused{0};
    std::int64_t last_status{0};
};

struct RateStats {
    double messages_per_second{0};
    std::uint64_t window_s{0};
    std::uint64_t venue_refusals{0};
    std::uint64_t broken_frames{0};
    std::uint64_t budget_denials{0};
    std::vector<RestLimitStats> rest_limits;
    std::vector<RestEndpointStats> rest_endpoints;
};

struct ProcessStats {
    std::int64_t pid{0};
    std::uint64_t generation{0};
    std::uint64_t started_ns{0};
    std::uint64_t uptime_s{0};
    std::string build;
};

struct GatewayStats {
    std::uint32_t version{k_stats_version};
    std::uint64_t taken_ns{0};
    std::string account;
    std::string venue;
    std::string market;
    std::string feed;
    std::vector<std::string> channels;
    ProcessStats process;
    CatalogueStats catalogue;
    std::vector<SubscriptionStats> subscriptions;
    std::vector<SocketStats> sockets;
    SocketLimits limits;
    CoverageStats coverage;
    RateStats rates;
    ClockStats clock;
    std::vector<LatencyStats> latency;
};

enum class Liveness : std::uint8_t { Up, Stale, Down };

[[nodiscard]] std::string_view to_string(Liveness state) noexcept;

// down wins over stale
[[nodiscard]] Liveness
classify(std::uint64_t taken_ns, bool process_alive, std::uint64_t now_ns, std::uint64_t max_age_ns) noexcept;

struct GatewayView {
    GatewayStats stats;
    Liveness state{Liveness::Up};
    std::uint64_t age_ms{0};
};

struct Rollup {
    std::string venue;
    std::uint64_t gateways{0};
    std::uint64_t down{0};
    std::uint64_t instruments{0};
    std::uint64_t subscriptions{0};
    std::uint64_t sockets{0};
    double messages_per_second{0};
};

struct MachineRollup {
    std::vector<Rollup> venues;
    Rollup total;
};

// a down gateway counts as down and adds nothing else
[[nodiscard]] MachineRollup rollup(std::span<const GatewayView> gateways);

struct MachineStats {
    std::uint64_t taken_ns{0};
    std::vector<GatewayView> gateways;
    MachineRollup rollup;
};

// messages per channel over a trailing window; fixed capacity
class RateWindow {
public:
    static constexpr std::size_t k_channels = 8;
    static constexpr std::size_t k_samples = 16;
    using Counts = std::array<std::uint64_t, k_channels>;

    explicit RateWindow(std::uint64_t window_ns) noexcept : window_ns_{window_ns} {}

    void push(std::uint64_t mono_ns, const Counts& counts) noexcept;
    [[nodiscard]] double per_second(std::size_t channel) const noexcept;
    [[nodiscard]] double total_per_second() const noexcept;

private:
    struct Sample {
        std::uint64_t mono_ns{0};
        Counts counts{};
    };

    [[nodiscard]] const Sample& newest() const noexcept;
    [[nodiscard]] const Sample& oldest_in_window() const noexcept;

    std::uint64_t window_ns_;
    std::array<Sample, k_samples> ring_{};
    std::size_t next_{0};
    std::size_t size_{0};
};

} // namespace raikiri::introspect
