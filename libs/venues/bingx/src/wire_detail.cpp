#include "wire_detail.hpp"

#include <cstring>

namespace raikiri::venues::bingx {

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

constexpr std::size_t k_initial_scratch = std::size_t{64} * 1024;

// /quote/contracts is roughly 470 KiB, so the ceiling has real headroom; past
// it a body is a venue incident.
constexpr std::size_t k_max_frame = std::size_t{64} * 1024 * 1024;

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

// raw_json() on a scalar returns the token plus whitespace up to the next one,
// so quotes come off after trimming. Contents stay escaped: BingX symbols and
// decimal strings cannot contain an escape, and letting simdjson unescape would
// move every field into a buffer that cannot be rebased.
std::expected<std::string_view, WireError> str_of(ondemand::value value) {
    std::string_view raw;
    if (const auto ec = value.raw_json().get(raw); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    raw = trim(raw);
    if (raw == "null") {
        return std::string_view{};
    }
    if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') {
        return std::unexpected(WireError::WrongType);
    }
    return session().rebase(raw.substr(1, raw.size() - 2));
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
    return str_of(value);
}

std::expected<std::string_view, WireError> req_str(ondemand::object& obj, std::string_view key) {
    ondemand::value value;
    if (const auto ec = obj.find_field_unordered(key).get(value); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    auto text = str_of(value);
    if (text && text->empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return text;
}

std::expected<std::uint64_t, WireError> opt_u64(ondemand::object& obj, std::string_view key) {
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

std::expected<std::int64_t, WireError> opt_i64(ondemand::object& obj, std::string_view key) {
    std::int64_t out = 0;
    const auto ec = obj.find_field_unordered(key).get_int64().get(out);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::int64_t{0};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return out;
}

std::expected<std::int64_t, WireError> req_i64(ondemand::object& obj, std::string_view key) {
    std::int64_t out = 0;
    if (const auto ec = obj.find_field_unordered(key).get_int64().get(out);
        ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return out;
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

std::expected<std::string_view, WireError> opt_raw(ondemand::object& obj, std::string_view key) {
    ondemand::value value;
    const auto ec = obj.find_field_unordered(key).get(value);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::string_view{};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    std::string_view raw;
    if (const auto raw_ec = value.raw_json().get(raw); raw_ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(raw_ec));
    }
    return session().rebase(trim(raw));
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

std::string_view symbol_of(std::string_view data_type) {
    const auto at = data_type.find('@');
    return at == std::string_view::npos ? std::string_view{} : data_type.substr(0, at);
}

} // namespace detail
} // namespace raikiri::venues::bingx
