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

} // namespace dto

namespace {

using dto::GatewayDto;
using dto::MachineDto;

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
    if (req.path.starts_with(k_stats) && req.path.size() > k_stats.size() + 1 &&
        req.path[k_stats.size()] == '/') {
        const auto account = req.path.substr(k_stats.size() + 1);
        auto view = (*gateway_)(account);
        if (!view.has_value()) {
            return http::problem(404, "no snapshot for that account", account);
        }
        return json(to_dto(std::move(*view)));
    }
    return http::problem(404, "no such path", req.path);
}

} // namespace raikiri::introspect
