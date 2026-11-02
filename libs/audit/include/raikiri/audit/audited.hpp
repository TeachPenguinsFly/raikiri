#pragma once

// wraps a control handler: what it was asked, and what it answered

#include <raikiri/audit/log.hpp>
#include <raikiri/http/server.hpp>

#include <functional>
#include <optional>
#include <string>

namespace raikiri::audit {

struct Command {
    std::string action{};
    std::string subject{};
};

// empty for a request that changes nothing; it sees the answer, since a create learns its id there
using Classify = std::function<std::optional<Command>(const http::Request&, const http::Response&)>;

// `sink` and `clock` must outlive the handler; `actor` is who the surface vouches for, not a checked identity
[[nodiscard]] http::Handler
audited(http::Handler inner, Sink& sink, const Clock& clock, std::string actor, Classify classify);

} // namespace raikiri::audit
