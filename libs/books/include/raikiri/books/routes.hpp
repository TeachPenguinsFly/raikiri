#pragma once

#include <raikiri/books/registry.hpp>
#include <raikiri/http/server.hpp>

#include <optional>

namespace raikiri::books {

// GET /v1/books; POST /v1/books opens `client:<name>`, and only that: the other kinds come from their own paths
class BookRoutes {
public:
    explicit BookRoutes(Registry& registry) : registry_{&registry} {}

    [[nodiscard]] std::optional<http::Response> operator()(const http::Request& req) const;

private:
    Registry* registry_;
};

} // namespace raikiri::books
