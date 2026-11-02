#pragma once

// the /risk paths on strategyd's control port; admind serves them as /v1/risk

#include <raikiri/bookrisk/desk.hpp>
#include <raikiri/http/server.hpp>

#include <optional>

namespace raikiri::bookrisk {

class RiskRoutes {
public:
    explicit RiskRoutes(RiskDesk& desk) : desk_{&desk} {}

    // empty outside /risk so a composed router can fall through
    [[nodiscard]] std::optional<http::Response> operator()(const http::Request& req) const;

private:
    RiskDesk* desk_;
};

} // namespace raikiri::bookrisk
