#pragma once

// The paths of schema/admin-api.yaml, resolved against a Registry.
//
// Separate from Server so the routing is testable without a socket: every
// answer below is a pure function of the registry and the request, which is
// the property that lets a contract test assert the JSON without binding a
// port.

#include <raikiri/admin/history.hpp>
#include <raikiri/admin/registry.hpp>
#include <raikiri/admin/server.hpp>

namespace raikiri::admin {

// Builds the handler the server calls. `now` is injected rather than read so
// liveness is testable; it is the MONOTONIC clock, because that is what a
// gateway stamps its heartbeat with.
//
// `history` may be null, and then every path that reads Postgres answers 503
// rather than an empty page: "nobody asked the database" and "the database
// holds nothing" are different facts, and a terminal that cannot tell them
// apart shows a flat account to someone who is not flat.
// `wall` is separate from `now` and both are injected: liveness is a monotonic
// question and staleness of a venue's own report is a wall-clock one, because
// that is the clock the report was stamped with. Reading one with the other is
// how a fresh snapshot came out looking fifty years old.
[[nodiscard]] Handler make_router(const Registry& registry,
                                  const History* history,
                                  std::function<MonoNanos()> now,
                                  std::function<UnixNanos()> wall);

// RFC 9457, rendered. The one error shape this API produces.
[[nodiscard]] Response problem(int status, std::string_view title, std::string_view detail = {});

} // namespace raikiri::admin
