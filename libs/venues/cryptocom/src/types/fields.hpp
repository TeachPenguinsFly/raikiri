#pragma once

// fields are walked in order; find_field_unordered asserts on malformed json

#include <raikiri/venues/cryptocom/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>

namespace raikiri::venues::cryptocom {

namespace ondemand = wire::ondemand;

using FieldResult = std::expected<void, WireError>;

template <typename Fn> [[nodiscard]] FieldResult each_field(ondemand::object& obj, Fn&& on_field) {
    for (auto entry : obj) {
        ondemand::field field;
        if (const auto ec = std::move(entry).get(field); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        if (auto done = on_field(field.escaped_key(), field.value()); !done.has_value()) {
            return done;
        }
    }
    return {};
}

[[nodiscard]] inline FieldResult text_into(ondemand::value& value, std::string_view& out) {
    const auto text = wire::str_of(value);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    out = *text;
    return {};
}

[[nodiscard]] inline FieldResult u64_into(ondemand::value& value, std::uint64_t& out) {
    if (const auto ec = value.get_uint64().get(out); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    return {};
}

[[nodiscard]] inline FieldResult i64_into(ondemand::value& value, std::int64_t& out) {
    if (const auto ec = value.get_int64().get(out); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    return {};
}

[[nodiscard]] inline FieldResult bool_into(ondemand::value& value, bool& out) {
    if (const auto ec = value.get_bool().get(out); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    return {};
}

// A view over the caller's buffer, not the parser's scratch.
[[nodiscard]] inline FieldResult raw_into(ondemand::value& value, std::string_view& out) {
    std::string_view raw;
    if (const auto ec = value.raw_json().get(raw); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    out = wire::session().rebase(wire::trim(raw));
    return {};
}

} // namespace raikiri::venues::cryptocom
