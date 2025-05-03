#include "wire_detail.hpp"

#include <charconv>

namespace raikiri::venues::deribit::detail {

std::expected<std::string_view, WireError> num_of(ondemand::value value) {
    std::string_view raw;
    if (const auto ec = value.raw_json().get(raw); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    raw = trim(raw);
    if (raw == "null") {
        return std::string_view{};
    }
    // A quoted token here means Deribit changed a number into a string.
    if (raw.empty() || raw.front() == '"') {
        return std::unexpected(WireError::WrongType);
    }
    return session().rebase(raw);
}

namespace {

// Shared spine of the opt_/req_ pairs; req_ adds a refusal of the empty case.
template <typename Reader>
[[nodiscard]] std::expected<std::string_view, WireError>
find_scalar(ondemand::object& obj, std::string_view key, Reader reader) {
    ondemand::value value;
    const auto ec = obj.find_field_unordered(key).get(value);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::string_view{};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return reader(value);
}

} // namespace

std::expected<std::string_view, WireError> opt_num(ondemand::object& obj, std::string_view key) {
    return find_scalar(obj, key, [](ondemand::value v) { return num_of(v); });
}

std::expected<std::string_view, WireError> req_num(ondemand::object& obj, std::string_view key) {
    const auto text = opt_num(obj, key);
    if (text.has_value() && text->empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return text;
}

std::uint64_t token_to_u64(std::string_view s) noexcept {
    if (s.empty()) {
        return 0;
    }
    std::uint64_t out = 0;
    const auto* const end = s.data() + s.size();
    const auto [stop, ec] = std::from_chars(s.data(), end, out);
    return (ec == std::errc{} && stop == end) ? out : 0;
}

std::expected<std::uint64_t, WireError> opt_ms(ondemand::object& obj, std::string_view key) {
    const auto token = opt_num(obj, key);
    if (!token.has_value()) {
        return std::unexpected(token.error());
    }
    return token_to_u64(*token);
}

} // namespace raikiri::venues::deribit::detail
