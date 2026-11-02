#pragma once

#include <raikiri/clientapi/application/ports.hpp>

#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace raikiri::clientapi::internal {

inline constexpr glz::opts k_write{.skip_null_members = true};
inline constexpr glz::opts k_lenient{.error_on_unknown_keys = false};

template <typename T> [[nodiscard]] std::string to_json(const T& value) {
    std::string out;
    if (glz::write<k_write>(value, out)) {
        return "{}";
    }
    return out;
}

// an unknown key in a client's body is refused: a misspelt reduce_only must not place a plain order
template <typename T> [[nodiscard]] bool from_body(T& value, std::string_view text) {
    return !glz::read_json(value, text);
}

// the backend may grow fields that are not part of this surface
template <typename T> [[nodiscard]] bool from_backend(T& value, std::string_view text) {
    return !glz::read<k_lenient>(value, text);
}

[[nodiscard]] inline bool all_digits(std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

} // namespace raikiri::clientapi::internal
