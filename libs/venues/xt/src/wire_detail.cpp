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

} // namespace raikiri::venues::xt::detail
