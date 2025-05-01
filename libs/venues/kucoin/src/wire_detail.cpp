#include "wire_detail.hpp"

#include <charconv>
#include <cstring>

namespace raikiri::venues::kucoin {

std::string_view to_string(WireError err) noexcept {
    switch (err) {
    case WireError::NotJson:
        return "NotJson";
    case WireError::MissingField:
        return "MissingField";
    case WireError::WrongType:
        return "WrongType";
    case WireError::TooManyEntries:
        return "TooManyEntries";
    }
    return "Unknown";
}

namespace detail {

namespace {

constexpr std::size_t k_initial_scratch = 64 * 1024;

// /api/v1/contracts/active is roughly 900 KiB across 677 rows, so the ceiling
// has real headroom; past it a frame is a venue incident or an attempt to make
// us allocate.
constexpr std::size_t k_max_frame = 64 * 1024 * 1024;

[[nodiscard]] constexpr bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

} // namespace

WireError map_error(simdjson::error_code ec) noexcept {
    switch (ec) {
    case simdjson::NO_SUCH_FIELD:
        return WireError::MissingField;
    case simdjson::INCORRECT_TYPE:
    case simdjson::NUMBER_ERROR:
    case simdjson::BIGINT_ERROR:
    case simdjson::NUMBER_OUT_OF_RANGE:
    case simdjson::SCALAR_DOCUMENT_AS_VALUE:
        return WireError::WrongType;
    default:
        return WireError::NotJson;
    }
}

std::expected<ondemand::object, WireError> Session::open_object(std::string_view frame) {
    if (frame.empty() || frame.size() > k_max_frame) {
        return std::unexpected(WireError::NotJson);
    }

    if (scratch_.length() < frame.size()) {
        std::size_t want = k_initial_scratch;
        while (want < frame.size()) {
            want *= 2;
        }
        simdjson::padded_string grown{want};
        if (grown.data() == nullptr) {
            return std::unexpected(WireError::NotJson);
        }
        scratch_ = std::move(grown);
    }

    std::memcpy(scratch_.data(), frame.data(), frame.size());
    std::memset(scratch_.data() + frame.size(), 0, simdjson::SIMDJSON_PADDING);

    origin_ = frame.data();
    origin_size_ = frame.size();

    const simdjson::padded_string_view view{
        scratch_.data(), frame.size(), frame.size() + simdjson::SIMDJSON_PADDING};
    if (const auto ec = parser_.iterate(view).get(doc_); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }

    ondemand::object root;
    if (const auto ec = doc_.get_object().get(root); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return root;
}

std::string_view Session::rebase(std::string_view scratch_view) const noexcept {
    const char* base = scratch_.data();
    if (base == nullptr || scratch_view.data() == nullptr) {
        return {};
    }
    if (scratch_view.data() < base || scratch_view.data() > base + origin_size_ ||
        static_cast<std::size_t>(scratch_view.data() - base) + scratch_view.size() > origin_size_) {
        return {};
    }
    return {origin_ + (scratch_view.data() - base), scratch_view.size()};
}

Session& session() noexcept {
    thread_local Session s;
    return s;
}

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && is_space(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && is_space(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

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

std::expected<std::string_view, WireError> str_of(ondemand::value value) {
    const auto raw = raw_of(value);
    if (!raw.has_value()) {
        return raw;
    }
    if (*raw == "null") {
        return std::string_view{};
    }
    if (raw->size() < 2 || raw->front() != '"' || raw->back() != '"') {
        return std::unexpected(WireError::WrongType);
    }
    return raw->substr(1, raw->size() - 2);
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

std::expected<std::string_view, WireError> opt_str(ondemand::object& obj, std::string_view key) {
    return lookup(obj, key, [](ondemand::value v) { return str_of(v); });
}

std::expected<std::string_view, WireError> req_str(ondemand::object& obj, std::string_view key) {
    auto text = opt_str(obj, key);
    if (text.has_value() && text->empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return text;
}

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

std::uint64_t digits_to_u64(std::string_view s) noexcept {
    if (s.empty()) {
        return 0;
    }
    std::uint64_t out = 0;
    const auto* const end = s.data() + s.size();
    const auto [stop, ec] = std::from_chars(s.data(), end, out);
    return (ec == std::errc{} && stop == end) ? out : 0;
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

std::expected<ondemand::array, WireError> req_array(ondemand::object& obj, std::string_view key) {
    ondemand::array out;
    if (const auto ec = obj.find_field_unordered(key).get_array().get(out);
        ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return out;
}

std::expected<ondemand::object, WireError> req_object(ondemand::object& obj, std::string_view key) {
    ondemand::object out;
    if (const auto ec = obj.find_field_unordered(key).get_object().get(out);
        ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return out;
}

} // namespace detail
} // namespace raikiri::venues::kucoin
