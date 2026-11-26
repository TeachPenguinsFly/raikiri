#pragma once

// venue decimals as doubles, for figures no ledger sums: fee rates and margin bracket bounds

#include <raikiri/wire/json.hpp>

#include <cmath>
#include <cstdlib>
#include <optional>
#include <string_view>

namespace raikiri::wire {

// plain or exponent notation, finite; nullopt for anything else, the empty string included
[[nodiscard]] inline std::optional<double> decimal_to_double(std::string_view text) noexcept {
    char buf[48];
    if (text.empty() || text.size() >= sizeof buf) {
        return std::nullopt;
    }
    for (const char c : text) {
        const bool digit = c >= '0' && c <= '9';
        if (!digit && c != '-' && c != '+' && c != '.' && c != 'e' && c != 'E') {
            return std::nullopt;
        }
    }
    text.copy(buf, text.size());
    buf[text.size()] = '\0';
    char* end = nullptr;
    const double value = std::strtod(buf, &end);
    if (end != buf + text.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

// a field holding a decimal as a string or as a bare number; `absent` stands in where it is missing or empty
[[nodiscard]] inline std::expected<double, Error>
decimal_field(ondemand::object& obj, std::string_view key, std::optional<double> absent = std::nullopt) {
    auto raw = opt_raw(obj, key);
    if (!raw.has_value()) {
        return std::unexpected(raw.error());
    }
    auto text = *raw;
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text = text.substr(1, text.size() - 2);
    }
    if (text.empty() || text == "null") {
        if (absent.has_value()) {
            return *absent;
        }
        return std::unexpected(map_error(simdjson::NO_SUCH_FIELD));
    }
    const auto value = decimal_to_double(text);
    if (!value.has_value()) {
        return std::unexpected(Error::WrongType);
    }
    return *value;
}

} // namespace raikiri::wire
