#pragma once

// /options/short-books on strategyd's control port, which alone knows what each book holds

#include <raikiri/http/server.hpp>
#include <raikiri/optionrisk/short_book.hpp>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace raikiri::optionrisk {

class ShortBookRoutes {
public:
    explicit ShortBookRoutes(std::function<std::vector<ShortBookRow>()> rows) : rows_{std::move(rows)} {}

    // empty on any other path so a composed router can fall through
    [[nodiscard]] std::optional<http::Response> operator()(const http::Request& req) const;

private:
    std::function<std::vector<ShortBookRow>()> rows_;
};

} // namespace raikiri::optionrisk
