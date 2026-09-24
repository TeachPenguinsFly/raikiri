#pragma once

#include <raikiri/admin/desk.hpp>
#include <raikiri/admin/history.hpp>
#include <raikiri/admin/registry.hpp>
#include <raikiri/http/server.hpp>

#include <map>
#include <string>

namespace raikiri::admin {

// `now` is monotonic and `wall` is wall clock; a null `history` or `desk` answers 503 on its paths
// `upstreams` maps a name to a loopback port; /v1/x/<name>/... is forwarded verbatim
[[nodiscard]] http::Handler make_router(const Registry& registry,
                                        const History* history,
                                        ManualDesk* desk,
                                        std::function<MonoNanos()> now,
                                        std::function<UnixNanos()> wall,
                                        std::map<std::string, std::uint16_t> upstreams = {});

// rfc 9457 problem response
[[nodiscard]] http::Response
problem(int status, std::string_view title, std::string_view detail = {});

} // namespace raikiri::admin
