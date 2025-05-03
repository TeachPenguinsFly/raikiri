#include "wire_detail.hpp"

namespace raikiri::venues::kraken::detail {

namespace {

[[nodiscard]] constexpr bool is_number_char(char c) noexcept {
    return (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' || c == 'e' || c == 'E';
}

} // namespace

std::expected<std::string_view, WireError> num_of(ondemand::value value) {
    std::string_view raw;
    if (const auto ec = value.raw_json().get(raw); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    raw = trim(raw);
    if (raw == "null") {
        return std::string_view{};
    }
    for (const char c : raw) {
        if (!is_number_char(c)) {
            return std::unexpected(WireError::WrongType);
        }
    }
    if (raw.empty()) {
        return std::unexpected(WireError::WrongType);
    }
    return session().rebase(raw);
}

std::expected<std::string_view, WireError> opt_num(ondemand::object& obj, std::string_view key) {
    ondemand::value value;
    const auto ec = obj.find_field_unordered(key).get(value);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::string_view{};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return num_of(value);
}

std::expected<std::string_view, WireError> req_num(ondemand::object& obj, std::string_view key) {
    ondemand::value value;
    if (const auto ec = obj.find_field_unordered(key).get(value); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    auto text = num_of(value);
    if (text && text->empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return text;
}

// `present` distinguishes an absent field from a real zero. That matters for
// contractValueTradePrecision, where 0 is a valid precision and defaulting to
// it would silently give an unknown instrument a step of exactly 1.
std::expected<std::int64_t, WireError>
opt_i64(ondemand::object& obj, std::string_view key, bool& present) {
    present = false;
    std::int64_t out = 0;
    const auto ec = obj.find_field_unordered(key).get_int64().get(out);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::int64_t{0};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    present = true;
    return out;
}

std::expected<std::uint64_t, WireError> opt_ms(ondemand::object& obj, std::string_view key) {
    const auto text = opt_num(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return digits_to_u64(*text);
}

} // namespace raikiri::venues::kraken::detail
