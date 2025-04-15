#include "wire_detail.hpp"

#include <charconv>
#include <cstring>

namespace raikiri::venues::gate {

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

// /api/v4/futures/usdt/contracts is roughly 1.1 MiB, so the ceiling has real
// headroom; past it a frame is a venue incident or an attempt to make us
// allocate.
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

std::expected<void, WireError> Session::load(std::string_view frame) {
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
    return {};
}

std::expected<ondemand::object, WireError> Session::open_object(std::string_view frame) {
    if (const auto ok = load(frame); !ok.has_value()) {
        return std::unexpected(ok.error());
    }
    ondemand::object root;
    if (const auto ec = doc_.get_object().get(root); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return root;
}

std::expected<ondemand::array, WireError> Session::open_array(std::string_view frame) {
    if (const auto ok = load(frame); !ok.has_value()) {
        return std::unexpected(ok.error());
    }
    ondemand::array root;
    if (const auto ec = doc_.get_array().get(root); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return root;
}

std::string_view Session::rebase(std::string_view scratch_view) const noexcept {
    const char* base = scratch_.data();
    if (base == nullptr || scratch_view.data() == nullptr) {
        return {};
    }
    // Outside the copy means simdjson materialised the bytes itself
    // (unescaped strings live in the parser's own buffer), and handing those
    // back would break the lifetime promise wire.hpp makes.
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

std::expected<std::string_view, WireError> str_of(ondemand::value value) {
    const auto raw = token_of(value);
    if (!raw.has_value()) {
        return std::unexpected(raw.error());
    }
    if (*raw == "null") {
        return std::string_view{};
    }
    if (raw->size() < 2 || raw->front() != '"' || raw->back() != '"') {
        return std::unexpected(WireError::WrongType);
    }
    return session().rebase(raw->substr(1, raw->size() - 2));
}

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
    // A bare token that is not a number (an object, an array, true) would
    // reach the decimal parsers as nonsense; refusing here keeps the failure
    // at the field that caused it.
    const char lead = raw->front();
    if (lead != '-' && lead != '+' && (lead < '0' || lead > '9')) {
        return std::unexpected(WireError::WrongType);
    }
    return session().rebase(*raw);
}

std::expected<std::string_view, WireError> opt_str(ondemand::object& obj, std::string_view key) {
    return find_scalar(obj, key, false, str_of);
}

std::expected<std::string_view, WireError> req_str(ondemand::object& obj, std::string_view key) {
    return find_scalar(obj, key, true, str_of);
}

std::expected<std::string_view, WireError> opt_num(ondemand::object& obj, std::string_view key) {
    return find_scalar(obj, key, false, num_of);
}

std::expected<std::string_view, WireError> req_num(ondemand::object& obj, std::string_view key) {
    return find_scalar(obj, key, true, num_of);
}

std::expected<bool, WireError> opt_bool(ondemand::object& obj, std::string_view key) {
    bool out = false;
    const auto ec = obj.find_field_unordered(key).get_bool().get(out);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return false;
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return out;
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
} // namespace raikiri::venues::gate
