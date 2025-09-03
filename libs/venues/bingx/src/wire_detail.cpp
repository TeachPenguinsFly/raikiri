#include "wire_detail.hpp"

namespace raikiri::venues::bingx::detail {

std::expected<std::int64_t, WireError> req_i64(ondemand::object& obj, std::string_view key) {
    std::int64_t out = 0;
    if (const auto ec = obj.find_field_unordered(key).get_int64().get(out);
        ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return out;
}

std::string_view symbol_of(std::string_view data_type) {
    const auto at = data_type.find('@');
    return at == std::string_view::npos ? std::string_view{} : data_type.substr(0, at);
}

// raw_json() on a scalar returns the token plus whitespace up to the next one,
// so quotes come off after trimming. Contents stay escaped: BingX symbols,
// order ids and decimal tokens cannot contain an escape, and letting simdjson
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
    }
    return session().rebase(raw);
}

std::expected<std::string_view, WireError> opt_scalar(ondemand::object& obj, std::string_view key) {
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

std::expected<std::string_view, WireError> req_scalar(ondemand::object& obj, std::string_view key) {
    const auto text = opt_scalar(obj, key);
    if (text.has_value() && text->empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return text;
}

std::expected<std::uint64_t, WireError> opt_ms_any(ondemand::object& obj, std::string_view key) {
    const auto text = opt_scalar(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return digits_to_u64(*text);
}

} // namespace raikiri::venues::bingx::detail
