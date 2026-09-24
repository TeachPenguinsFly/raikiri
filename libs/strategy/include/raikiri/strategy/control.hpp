#pragma once

// served on strategyd's --port through admind's /v1/x/<name>/
// the handler runs strategy code, so only the strategy loop may poll its server

#include <raikiri/http/server.hpp>
#include <raikiri/portfolio/position_book.hpp>
#include <raikiri/strategy/factory.hpp>
#include <raikiri/strategy/registry.hpp>

#include <stop_token>

namespace raikiri::strategy {

// `live`, `types` and `book` must outlive the handler; `stop` is the process's stop signal
[[nodiscard]] http::Handler make_control(Registry& live,
                                         const Factory& types,
                                         const portfolio::PositionBook& book,
                                         std::stop_token stop);

} // namespace raikiri::strategy
