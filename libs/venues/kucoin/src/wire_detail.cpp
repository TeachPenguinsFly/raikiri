#include "wire_detail.hpp"

#include <charconv>

namespace raikiri::venues::kucoin::detail {

// raw_json() returns the token plus whitespace up to the next one. Contents
// stay escaped: KuCoin symbols, order ids and decimal strings cannot contain an
// escape, and letting simdjson unescape would move every field into a buffer
// that cannot be rebased.
std::expected<std::string_view, WireError> raw_of(ondemand::value value) {
    std::string_view raw;
    if (const auto ec = value.raw_json().get(raw); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return session().rebase(trim(raw));
}

namespace {

// Shared body of the opt_/req_ pairs; an absent field reads as empty.
template <typename Read>
[[nodiscard]] std::expected<std::string_view, WireError>
lookup(ondemand::object& obj, std::string_view key, Read read) {
    ondemand::value value;
    const auto ec = obj.find_field_unordered(key).get(value);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::string_view{};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return read(value);
}

} // namespace

std::expected<std::string_view, WireError> opt_decimal(ondemand::object& obj,
                                                       std::string_view key) {
    return lookup(obj, key, [](ondemand::value v) -> std::expected<std::string_view, WireError> {
        const auto raw = raw_of(v);
        if (!raw.has_value()) {
            return raw;
        }
        if (*raw == "null") {
            return std::string_view{};
        }
        if (raw->size() >= 2 && raw->front() == '"' && raw->back() == '"') {
            return raw->substr(1, raw->size() - 2);
        }
        return *raw;
    });
}

std::expected<std::string_view, WireError> req_decimal(ondemand::object& obj,
                                                       std::string_view key) {
    auto text = opt_decimal(obj, key);
    if (text.has_value() && text->empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return text;
}

std::expected<std::uint64_t, WireError> opt_u64(ondemand::object& obj, std::string_view key) {
    const auto text = opt_decimal(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return digits_to_u64(*text);
}

std::expected<std::int64_t, WireError> opt_i64(ondemand::object& obj, std::string_view key) {
    const auto text = opt_decimal(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    std::int64_t out = 0;
    const auto* const end = text->data() + text->size();
    const auto [stop, ec] = std::from_chars(text->data(), end, out);
    return (ec == std::errc{} && stop == end) ? out : std::int64_t{0};
}

} // namespace raikiri::venues::kucoin::detail
