#include "wire_detail.hpp"

namespace raikiri::venues::xt::detail {

std::expected<std::int64_t, WireError> req_i64(ondemand::object& obj, std::string_view key) {
    std::int64_t out = 0;
    if (const auto ec = obj.find_field_unordered(key).get_int64().get(out);
        ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return out;
}

std::expected<bool, WireError> req_bool(ondemand::object& obj, std::string_view key) {
    bool out = false;
    if (const auto ec = obj.find_field_unordered(key).get_bool().get(out);
        ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return out;
}

std::expected<std::uint64_t, WireError> opt_ms(ondemand::object& obj, std::string_view key) {
    std::uint64_t out = 0;
    const auto ec = obj.find_field_unordered(key).get_uint64().get(out);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::uint64_t{0};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return out;
}

std::expected<std::uint64_t, WireError> req_digits(ondemand::object& obj, std::string_view key) {
    const auto text = req_str(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    const auto value = digits_to_u64(*text);
    if (value == 0) {
        return std::unexpected(WireError::WrongType);
    }
    return value;
}

// raw_json() on a scalar returns the token plus whitespace up to the next one,
// so quotes come off after trimming. Contents stay escaped: XT symbols, order
// ids and decimal tokens cannot contain an escape, and letting simdjson
// unescape would move every field into a buffer that cannot be rebased.
std::expected<std::string_view, WireError> scalar_of(ondemand::value value) {
    std::string_view raw;
    if (const auto ec = value.raw_json().get(raw); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    raw = trim(raw);
    if (raw == "null") {
        return std::string_view{};
    }
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        raw = raw.substr(1, raw.size() - 2);
    } else if (raw.empty() || raw.front() == '{' || raw.front() == '[') {
        return std::unexpected(WireError::WrongType);
    }
    return session().rebase(raw);
}

std::expected<std::string_view, WireError> opt_scalar(ondemand::object& obj,
                                                      std::string_view key) {
    ondemand::value value;
    const auto ec = obj.find_field_unordered(key).get(value);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::string_view{};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return scalar_of(value);
}

std::expected<std::int64_t, WireError>
opt_i64(ondemand::object& obj, std::string_view key, std::int64_t fallback) {
    auto field = obj.find_field_unordered(key);
    if (field.error() == simdjson::NO_SUCH_FIELD) {
        return fallback;
    }
    if (field.error() != simdjson::SUCCESS) {
        return std::unexpected(map_error(field.error()));
    }
    // An explicit null is the venue saying "no value", which is what a fallback
    // is for. A wrong TYPE is not: XT spells some numbers as strings, and
    // folding that into the fallback would drop a value the venue did send.
    if (field.is_null().value_unsafe()) {
        return fallback;
    }
    std::int64_t out = 0;
    if (const auto ec = field.get_int64().get(out); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return out;
}

std::expected<bool, WireError>
opt_bool_or(ondemand::object& obj, std::string_view key, bool fallback) {
    auto field = obj.find_field_unordered(key);
    if (field.error() == simdjson::NO_SUCH_FIELD) {
        return fallback;
    }
    if (field.error() == simdjson::SUCCESS && field.is_null().value_unsafe()) {
        return fallback;
    }
    bool out = false;
    const auto ec = field.get_bool().get(out);
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return out;
}

} // namespace raikiri::venues::xt::detail
