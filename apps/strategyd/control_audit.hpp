#pragma once

// which requests on the control port change something, under what name

#include <raikiri/audit/audited.hpp>
#include <raikiri/http/server.hpp>

#include <cstdint>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace raikiri::control_audit {

struct CreatedParent {
    std::string id{};
};

struct CreatedStrategy {
    std::int64_t strategy_id{-1};
};

constexpr glz::opts k_partial{.error_on_unknown_keys = false};

} // namespace raikiri::control_audit

[[nodiscard]] inline std::optional<raikiri::audit::Command> control_command(const raikiri::http::Request& req,
                                                                            const raikiri::http::Response& answer) {
    using raikiri::audit::Command;
    using namespace raikiri::control_audit;
    if (req.method != "POST") {
        return std::nullopt;
    }
    if (req.path == "/v1/strategies") {
        CreatedStrategy body{};
        // the answer is absent or a problem on refusal; the request names the id either way
        (void)glz::read<k_partial>(body, req.body);
        return Command{.action = "strategy.create",
                       .subject = body.strategy_id < 0 ? std::string{"strategy:?"}
                                                       : "strategy:" + std::to_string(body.strategy_id)};
    }
    if (req.path == "/joe/parents") {
        CreatedParent body{};
        if (answer.status == 201) {
            (void)glz::read<k_partial>(body, answer.body);
        }
        return Command{.action = "joe.parent.create",
                       .subject = body.id.empty() ? std::string{"parent:new"} : "parent:" + body.id};
    }
    const auto verb_of =
        [&req](std::string_view prefix) -> std::optional<std::pair<std::string_view, std::string_view>> {
        if (!req.path.starts_with(prefix)) {
            return std::nullopt;
        }
        const auto tail = req.path.substr(prefix.size());
        const auto slash = tail.find('/');
        if (slash == std::string_view::npos) {
            return std::nullopt;
        }
        return std::pair{tail.substr(0, slash), tail.substr(slash + 1)};
    };
    if (const auto found = verb_of("/v1/strategies/"); found.has_value()) {
        if (found->second == "start" || found->second == "stop") {
            return Command{.action = "strategy." + std::string{found->second},
                           .subject = "strategy:" + std::string{found->first}};
        }
        return std::nullopt;
    }
    if (const auto found = verb_of("/joe/parents/"); found.has_value()) {
        if (found->second == "cancel" || found->second == "pause" || found->second == "resume") {
            return Command{.action = "joe.parent." + std::string{found->second},
                           .subject = "parent:" + std::string{found->first}};
        }
    }
    return std::nullopt;
}
