#pragma once

// /risk/internal on strategyd's control port; admind serves it as /v1/risk/internal

#include <raikiri/http/server.hpp>
#include <raikiri/internalcross/application/control.hpp>
#include <raikiri/internalcross/domain/liability.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace raikiri::internalcross {

struct LiabilityView {
    LiabilityRow row{};
    std::string symbol{};
    // the position the venue last reported; nullopt when it has not answered
    std::optional<std::int64_t> venue{};
};

class CrossRoutes {
public:
    using Liabilities = std::function<std::vector<LiabilityView>()>;
    using BookName = std::function<std::string(std::uint16_t)>;

    CrossRoutes(CrossControl& control, Liabilities liabilities, BookName book_name)
        : control_{&control}, liabilities_{std::move(liabilities)}, book_name_{std::move(book_name)} {}

    // empty outside /risk/internal, so a composed router falls through
    [[nodiscard]] std::optional<http::Response> operator()(const http::Request& req) const;

private:
    CrossControl* control_;
    Liabilities liabilities_;
    BookName book_name_;
};

} // namespace raikiri::internalcross
