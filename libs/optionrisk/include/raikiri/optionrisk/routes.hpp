#pragma once

// /v1/options/risk and /v1/options/risk/scenarios of schema/admin-api.yaml

#include <raikiri/http/server.hpp>
#include <raikiri/optionrisk/queries.hpp>

#include <optional>

namespace raikiri::optionrisk {

class RiskRoutes {
public:
    explicit RiskRoutes(OptionRisk& risk) : risk_{&risk} {}

    // empty on any other path so a composed router can fall through
    [[nodiscard]] std::optional<http::Response> operator()(const http::Request& req) const;

private:
    OptionRisk* risk_;
};

} // namespace raikiri::optionrisk
