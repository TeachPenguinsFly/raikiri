#pragma once

// GET /v1/audit and GET /v1/audit/status of schema/admin-api.yaml

#include <raikiri/audit/query.hpp>
#include <raikiri/http/server.hpp>

#include <optional>

namespace raikiri::audit {

class AuditRoutes {
public:
    AuditRoutes(const QueryAudit& query, const GetChainStatus& status) : query_{&query}, status_{&status} {}

    // empty outside /v1/audit so a composed router can fall through
    [[nodiscard]] std::optional<http::Response> operator()(const http::Request& req) const;

private:
    const QueryAudit* query_;
    const GetChainStatus* status_;
};

} // namespace raikiri::audit
