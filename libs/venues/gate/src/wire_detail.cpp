#include "wire_detail.hpp"

#include <charconv>

namespace raikiri::venues::gate::detail {

namespace {

// raw_json() on a scalar returns the token plus whitespace up to the next one.
// Contents stay escaped: Gate contract names and decimal strings cannot
// contain an escape, and letting simdjson unescape would move every field into
// a buffer that cannot be rebased.
std::expected<std::string_view, WireError> token_of(ondemand::value value) {
    std::string_view raw;
    if (const auto ec = value.raw_json().get(raw); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return trim(raw);
}

template <typename Fn>
std::expected<std::string_view, WireError>
find_scalar(ondemand::object& obj, std::string_view key, bool required, Fn&& read) {
    ondemand::value value;
    const auto ec = obj.find_field_unordered(key).get(value);
    if (ec == simdjson::NO_SUCH_FIELD) {
        if (required) {
            return std::unexpected(WireError::MissingField);
        }
        return std::string_view{};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    auto text = std::forward<Fn>(read)(value);
    if (required && text.has_value() && text->empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return text;
}

} // namespace

std::expected<std::string_view, WireError> num_of(ondemand::value value) {
    const auto raw = token_of(value);
    if (!raw.has_value()) {
        return std::unexpected(raw.error());
    }
    if (*raw == "null") {
        return std::string_view{};
    }
    if (raw->size() >= 2 && raw->front() == '"' && raw->back() == '"') {
        return session().rebase(raw->substr(1, raw->size() - 2));
    }
    const char lead = raw->front();
    if (lead != '-' && lead != '+' && (lead < '0' || lead > '9')) {
        return std::unexpected(WireError::WrongType);
    }
    return session().rebase(*raw);
}

std::expected<std::string_view, WireError> opt_num(ondemand::object& obj, std::string_view key) {
    return find_scalar(obj, key, false, num_of);
}

std::expected<std::string_view, WireError> req_num(ondemand::object& obj, std::string_view key) {
    return find_scalar(obj, key, true, num_of);
}

std::uint64_t whole_u64(std::string_view s) noexcept {
    if (s.empty()) {
        return 0;
    }
    std::uint64_t out = 0;
    const auto* const end = s.data() + s.size();
    const auto [stop, ec] = std::from_chars(s.data(), end, out);
    if (ec != std::errc{}) {
        return 0;
    }
    if (stop == end) {
        return out;
    }
    return *stop == '.' ? out : 0;
}

std::expected<std::uint64_t, WireError> opt_u64(ondemand::object& obj, std::string_view key) {
    const auto text = opt_num(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return whole_u64(*text);
}

} // namespace raikiri::venues::gate::detail
