#include <raikiri/exec/venue_clock.hpp>

#include <charconv>

namespace raikiri::exec {

namespace {

std::optional<std::size_t> value_at(std::string_view body, std::string_view key) noexcept {
    std::size_t at = 0;
    if (!key.empty()) {
        for (;;) {
            at = body.find(key, at);
            if (at == std::string_view::npos) {
                return std::nullopt;
            }
            const auto end = at + key.size();
            if (at > 0 && body[at - 1] == '"' && end < body.size() && body[end] == '"') {
                at = end + 1;
                break;
            }
            at = end;
        }
    }
    while (at < body.size() && (body[at] == ' ' || body[at] == ':' || body[at] == '"' || body[at] == '\n')) {
        ++at;
    }
    return at;
}

} // namespace

std::optional<std::uint64_t> json_number(std::string_view body, std::string_view key, std::uint64_t unit_ns) noexcept {
    const auto at = value_at(body, key);
    if (!at.has_value()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto* first = body.data() + *at;
    const auto [ptr, ec] = std::from_chars(first, body.data() + body.size(), value);
    if (ec != std::errc{} || ptr == first || value == 0) {
        return std::nullopt;
    }
    return value * unit_ns;
}

std::optional<std::uint64_t> json_seconds(std::string_view body, std::string_view key) noexcept {
    const auto at = value_at(body, key);
    if (!at.has_value()) {
        return std::nullopt;
    }
    const auto* last = body.data() + body.size();
    std::uint64_t whole = 0;
    const auto* first = body.data() + *at;
    auto [ptr, ec] = std::from_chars(first, last, whole);
    if (ec != std::errc{} || ptr == first || whole == 0) {
        return std::nullopt;
    }
    std::uint64_t fraction = 0;
    std::uint64_t scale = 1'000'000'000ULL;
    if (ptr != last && *ptr == '.') {
        for (++ptr; ptr != last && *ptr >= '0' && *ptr <= '9'; ++ptr) {
            if (scale > 1) {
                scale /= 10;
                fraction += static_cast<std::uint64_t>(*ptr - '0') * scale;
            }
        }
    }
    return whole * 1'000'000'000ULL + fraction;
}

} // namespace raikiri::exec
