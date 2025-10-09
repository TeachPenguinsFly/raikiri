#pragma once

// The strategy service's own HTTP surface: what is running, what it holds, and
// the two instructions that change either.
//
// Served on strategyd's --port and reached through admind at
// /v1/x/<name>/..., so NOTHING NEEDS ADDING TO ADMIND: it forwards a path it
// does not understand to a port it was told about, which is the whole
// arrangement that keeps a private strategy's shapes out of this repository.
//
// NO STRATEGY TYPE IS NAMED BELOW. /v1/strategy-types answers with whatever
// was registered at runtime, so a service linking strategies this repository
// does not contain lists them without a line changing here.
//
// Separate from Server so the routing is testable without a socket, exactly as
// admin::make_router is: every answer is a pure function of the registry, the
// book and the request.
//
// THE HANDLER RUNS STRATEGY CODE — a start calls on_start, a stop calls
// on_stop — so it may only be installed on a server the strategy loop itself
// polls. http::Server is single-threaded by design, which makes that true by
// construction as long as nobody polls this one from a second thread.

#include <raikiri/http/server.hpp>
#include <raikiri/portfolio/position_book.hpp>
#include <raikiri/strategy/factory.hpp>
#include <raikiri/strategy/registry.hpp>

#include <stop_token>

namespace raikiri::strategy {

// `live`, `types` and `book` are borrowed and must outlive the handler.
//
// `stop` is the PROCESS's stop signal, handed to every instance this surface
// starts. Without it a strategy started over HTTP would be one that shutting
// the process down never told to stop — it would be torn down by nothing, and
// its quotes would outlive the kernel that placed them.
[[nodiscard]] http::Handler make_control(Registry& live,
                                         const Factory& types,
                                         const portfolio::PositionBook& book,
                                         std::stop_token stop);

} // namespace raikiri::strategy
