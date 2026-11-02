#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace raikiri::clientapi {

// `code` is the stable, machine-read name of the refusal; title and detail may change
struct Refusal {
    int status{500};
    std::string code{};
    std::string title{};
    std::string detail{};
    std::uint32_t retry_after_s{0};
    // the backend took the call and never answered, so it may have happened
    bool outcome_unknown{false};
};

template <typename T> using Outcome = std::expected<T, Refusal>;

} // namespace raikiri::clientapi
