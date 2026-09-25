#pragma once

// the questions admind answers and the one a gateway sends, each against a port

#include <raikiri/introspect/stats.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace raikiri::introspect {

struct SnapshotRecord {
    GatewayStats stats;
    bool process_alive{true};
};

class GatewayStatsSource {
public:
    GatewayStatsSource() = default;
    GatewayStatsSource(const GatewayStatsSource&) = delete;
    GatewayStatsSource& operator=(const GatewayStatsSource&) = delete;
    virtual ~GatewayStatsSource() = default;

    [[nodiscard]] virtual std::vector<SnapshotRecord> list() const = 0;
    [[nodiscard]] virtual std::optional<SnapshotRecord> read(std::string_view account) const = 0;
};

class StatsPublisher {
public:
    StatsPublisher() = default;
    StatsPublisher(const StatsPublisher&) = delete;
    StatsPublisher& operator=(const StatsPublisher&) = delete;
    virtual ~StatsPublisher() = default;

    [[nodiscard]] virtual bool publish(const GatewayStats& stats) = 0;
};

// wall-clock nanoseconds, the clock a snapshot is stamped with
using WallClock = std::function<std::uint64_t()>;

inline constexpr std::uint64_t k_stale_after_ns = 30ULL * 1'000'000'000ULL;

class GetGatewayStats {
public:
    GetGatewayStats(const GatewayStatsSource& source, WallClock now, std::uint64_t stale_after_ns = k_stale_after_ns);

    [[nodiscard]] std::optional<GatewayView> operator()(std::string_view account) const;

private:
    friend class GetMachineStats;
    [[nodiscard]] GatewayView view(SnapshotRecord record, std::uint64_t now_ns) const;

    const GatewayStatsSource* source_;
    WallClock now_;
    std::uint64_t stale_after_ns_;
};

class GetMachineStats {
public:
    GetMachineStats(const GatewayStatsSource& source, WallClock now, std::uint64_t stale_after_ns = k_stale_after_ns);

    [[nodiscard]] MachineStats operator()() const;

private:
    GetGatewayStats one_;
};

} // namespace raikiri::introspect
