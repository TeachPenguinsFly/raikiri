#pragma once

// PRESENTATION: the /cache/ namespace of schema/admin-api.yaml. Debug views
// read from what the processes already know, never from Postgres.

#include <raikiri/http/server.hpp>
#include <raikiri/introspect/queries.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace raikiri::introspect {

inline constexpr std::string_view k_cache_prefix = "/cache/";

class CacheRoutes {
public:
    CacheRoutes(const GetMachineStats& machine, const GetGatewayStats& gateway)
        : machine_{&machine}, gateway_{&gateway} {}

    // Empty for a path outside /cache/, so a composed router can fall through.
    [[nodiscard]] std::optional<http::Response> operator()(const http::Request& req) const;

private:
    const GetMachineStats* machine_;
    const GetGatewayStats* gateway_;
};

inline constexpr std::string_view k_metrics_path = "/metrics";

// Prometheus text exposition 0.0.4: latency as cumulative histograms with
// fixed log buckets, so they add up across gateways and machines; clock
// offsets as gauges. The windowed quantiles stay in /cache/latency.
[[nodiscard]] std::string prometheus_text(const MachineStats& machine);

class MetricsRoute {
public:
    explicit MetricsRoute(const GetMachineStats& machine) : machine_{&machine} {}

    [[nodiscard]] std::optional<http::Response> operator()(const http::Request& req) const;

private:
    const GetMachineStats* machine_;
};

} // namespace raikiri::introspect
