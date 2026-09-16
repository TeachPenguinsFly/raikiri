#include "json_mapping.hpp"

#include <raikiri/introspect/routes.hpp>

#include <string>
#include <utility>
#include <vector>

namespace raikiri::introspect {

namespace dto {

struct GatewayDto {
    std::string account;
    std::string state;
    bool stale{false};
    std::uint64_t age_ms{0};
    GatewayStats snapshot;
};

struct MachineDto {
    std::string taken_ns;
    std::vector<GatewayDto> gateways;
    MachineRollup rollup;
};

struct LatencyDto {
    std::string account;
    std::string venue;
    std::string market;
    std::string state;
    std::uint64_t age_ms{0};
    ClockStats clock;
    std::vector<LatencyStats> latency;
};

struct LatencyMachineDto {
    std::string taken_ns;
    std::vector<LatencyDto> gateways;
};

} // namespace dto

namespace {

using dto::GatewayDto;
using dto::LatencyDto;
using dto::LatencyMachineDto;
using dto::MachineDto;

LatencyDto to_latency(GatewayView view) {
    return LatencyDto{.account = std::move(view.stats.account),
                      .venue = std::move(view.stats.venue),
                      .market = std::move(view.stats.market),
                      .state = std::string{to_string(view.state)},
                      .age_ms = view.age_ms,
                      .clock = std::move(view.stats.clock),
                      .latency = std::move(view.stats.latency)};
}

[[nodiscard]] std::optional<std::string_view> account_under(std::string_view path,
                                                            std::string_view prefix) {
    if (path.starts_with(prefix) && path.size() > prefix.size() + 1 && path[prefix.size()] == '/') {
        return path.substr(prefix.size() + 1);
    }
    return std::nullopt;
}

GatewayDto to_dto(GatewayView view) {
    auto account = view.stats.account;
    return GatewayDto{.account = std::move(account),
                      .state = std::string{to_string(view.state)},
                      .stale = view.state == Liveness::Stale,
                      .age_ms = view.age_ms,
                      .snapshot = std::move(view.stats)};
}

template <typename T> http::Response json(const T& value) {
    std::string out;
    if (glz::write_json(value, out)) {
        return http::problem(500, "serialisation failed");
    }
    return {.status = 200, .content_type = "application/json", .body = std::move(out)};
}

} // namespace

std::optional<http::Response> CacheRoutes::operator()(const http::Request& req) const {
    if (!req.path.starts_with(k_cache_prefix)) {
        return std::nullopt;
    }
    if (req.method != "GET") {
        return http::problem(405, "method not allowed", "this path serves reads");
    }
    static constexpr std::string_view k_stats = "/cache/stats";
    if (req.path == k_stats) {
        auto machine = (*machine_)();
        MachineDto dto{.taken_ns = std::to_string(machine.taken_ns),
                       .gateways = {},
                       .rollup = std::move(machine.rollup)};
        dto.gateways.reserve(machine.gateways.size());
        for (auto& view : machine.gateways) {
            dto.gateways.push_back(to_dto(std::move(view)));
        }
        return json(dto);
    }
    if (const auto account = account_under(req.path, k_stats); account.has_value()) {
        auto view = (*gateway_)(*account);
        if (!view.has_value()) {
            return http::problem(404, "no snapshot for that account", *account);
        }
        return json(to_dto(std::move(*view)));
    }
    static constexpr std::string_view k_latency = "/cache/latency";
    if (req.path == k_latency) {
        auto machine = (*machine_)();
        LatencyMachineDto dto{.taken_ns = std::to_string(machine.taken_ns), .gateways = {}};
        dto.gateways.reserve(machine.gateways.size());
        for (auto& view : machine.gateways) {
            dto.gateways.push_back(to_latency(std::move(view)));
        }
        return json(dto);
    }
    if (const auto account = account_under(req.path, k_latency); account.has_value()) {
        auto view = (*gateway_)(*account);
        if (!view.has_value()) {
            return http::problem(404, "no snapshot for that account", *account);
        }
        return json(to_latency(std::move(*view)));
    }
    return http::problem(404, "no such path", req.path);
}

std::optional<http::Response> MetricsRoute::operator()(const http::Request& req) const {
    if (req.path != k_metrics_path) {
        return std::nullopt;
    }
    if (req.method != "GET") {
        return http::problem(405, "method not allowed", "this path serves reads");
    }
    return http::Response{.status = 200,
                          .content_type = "text/plain; version=0.0.4; charset=utf-8",
                          .body = prometheus_text((*machine_)())};
}

} // namespace raikiri::introspect
