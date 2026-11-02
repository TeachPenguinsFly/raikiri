#include <raikiri/books/routes.hpp>

#include <glaze/glaze.hpp>
#include <string>
#include <vector>

namespace raikiri::books {

namespace dto {

struct NewBook {
    std::string name;
};

struct BookRow {
    std::uint16_t id{0};
    std::string kind;
    std::string name;
    std::string created;
};

} // namespace dto

namespace internal {

[[nodiscard]] dto::BookRow row(const Book& book) {
    return {.id = book.id,
            .kind = std::string{to_string(book.kind)},
            .name = book.name,
            .created = std::to_string(book.created.ns)};
}

template <typename T> [[nodiscard]] http::Response json(const T& value, int status) {
    std::string body;
    if (glz::write_json(value, body)) {
        return http::problem(500, "will not serialize");
    }
    return {.status = status, .content_type = "application/json", .body = std::move(body)};
}

} // namespace internal

std::optional<http::Response> BookRoutes::operator()(const http::Request& req) const {
    if (req.path != "/v1/books") {
        return std::nullopt;
    }
    if (req.method == "GET") {
        registry_->refresh();
        std::vector<dto::BookRow> rows;
        for (const auto& book : registry_->list()) {
            rows.push_back(internal::row(book));
        }
        return internal::json(rows, 200);
    }
    if (req.method != "POST") {
        return http::problem(405, "method not allowed");
    }
    dto::NewBook body{};
    if (glz::read_json(body, req.body)) {
        return http::problem(400, "malformed body");
    }
    if (reserved_kind(body.name) != BookKind::Client) {
        return http::problem(422, "only a client's book opens here", to_string(BookError::BadName));
    }
    registry_->refresh();
    const bool held = registry_->find(body.name) != nullptr;
    const auto opened = registry_->open(body.name);
    if (!opened.has_value()) {
        return http::problem(503, "refused", to_string(opened.error()));
    }
    return internal::json(internal::row(*opened), held ? 200 : 201);
}

} // namespace raikiri::books
