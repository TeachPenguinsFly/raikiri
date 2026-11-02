#pragma once

// the /joe/ paths on strategyd's control port; admind serves them as /v1/joe/

#include <raikiri/books/registry.hpp>
#include <raikiri/http/server.hpp>
#include <raikiri/joe/desk.hpp>

#include <optional>
#include <string_view>

namespace raikiri::joe {

inline constexpr std::string_view k_prefix = "/joe/";

class JoeRoutes {
public:
    // a null desk answers 503: strategyd runs without --joe
    // a null registry can start a parent only on joe's own book
    explicit JoeRoutes(Desk* desk, books::Registry* books = nullptr) : desk_{desk}, books_{books} {}

    // empty outside /joe/ so a composed router can fall through
    [[nodiscard]] std::optional<http::Response> operator()(const http::Request& req) const;

private:
    Desk* desk_;
    books::Registry* books_;
};

} // namespace raikiri::joe
