#pragma once

// GET /v1/reference of schema/admin-api.yaml, read from the ref.mid segment

#include <raikiri/http/server.hpp>
#include <raikiri/reference/engine.hpp>
#include <raikiri/reference/layout.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace raikiri::reference {

inline constexpr std::string_view k_reference_path = "/v1/reference";

// a ratio e9 as basis points with two decimals: 1'234'567 is "12.34"
[[nodiscard]] std::string bp_text(std::int64_t ratio_e9);

class ReferenceRoute {
public:
    ReferenceRoute(ReferenceFeed& feed, WallClock wall) : feed_{&feed}, wall_{std::move(wall)} {}

    // empty off its path so a composed router falls through
    [[nodiscard]] std::optional<http::Response> operator()(const http::Request& req) const;

private:
    ReferenceFeed* feed_;
    WallClock wall_;
};

} // namespace raikiri::reference
