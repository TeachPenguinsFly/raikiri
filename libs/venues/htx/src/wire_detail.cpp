#include "wire_detail.hpp"

namespace raikiri::venues::htx::detail {

// raw_json() on a scalar returns the token plus whitespace up to the next one,
// so the trim is what makes the token exact. Contents stay escaped: HTX
// contract codes and decimal tokens cannot contain an escape, and letting
// simdjson unescape would move every field into a buffer that cannot be
// rebased.
std::expected<std::string_view, WireError> raw_of(ondemand::value value) {
    std::string_view raw;
    if (const auto ec = value.raw_json().get(raw); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return session().rebase(trim(raw));
}

std::string_view unquote(std::string_view raw) noexcept {
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        return raw.substr(1, raw.size() - 2);
    }
    return raw;
}

std::expected<std::uint64_t, WireError> opt_ms_str(ondemand::object& obj, std::string_view key) {
    const auto text = opt_str(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return digits_to_u64(*text);
}

std::string_view code_of(std::string_view topic) {
    const auto first = topic.find('.');
    if (first == std::string_view::npos) {
        return {};
    }
    const auto second = topic.find('.', first + 1);
    if (second == std::string_view::npos) {
        return {};
    }
    return topic.substr(first + 1, second - first - 1);
}

} // namespace raikiri::venues::htx::detail
