#include "wire_detail.hpp"

#include <charconv>

namespace raikiri::venues::bitfinex::detail {

bool is_array_frame(std::string_view frame) noexcept {
    const auto text = trim(frame);
    return !text.empty() && text.front() == '[';
}

// raw_json() on a scalar returns the token plus whitespace up to the next one,
// so quotes come off after trimming. Contents stay escaped: Bitfinex symbols
// and decimal tokens cannot contain an escape, and letting simdjson unescape
// would move every field into a buffer that cannot be rebased.
std::expected<std::string_view, WireError> token_of(ondemand::value value) {
    std::string_view raw;
    if (const auto ec = value.raw_json().get(raw); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    raw = trim(raw);
    if (raw == "null") {
        return std::string_view{};
    }
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        return session().rebase(raw.substr(1, raw.size() - 2));
    }
    return session().rebase(raw);
}

bool token_to_i64(std::string_view s, std::int64_t& out) noexcept {
    if (s.empty()) {
        return false;
    }
    const auto* const end = s.data() + s.size();
    const auto [stop, ec] = std::from_chars(s.data(), end, out);
    return ec == std::errc{} && stop == end;
}

std::expected<std::string_view, WireError> opt_str(ondemand::object& obj, std::string_view key) {
    ondemand::value value;
    const auto ec = obj.find_field_unordered(key).get(value);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::string_view{};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return token_of(value);
}

} // namespace raikiri::venues::bitfinex::detail
