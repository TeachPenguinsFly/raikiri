#include <raikiri/wire/json.hpp>

#include <charconv>
#include <cstring>

namespace raikiri::wire {

std::string_view to_string(Error err) noexcept {
    switch (err) {
    case Error::NotJson:
        return "NotJson";
    case Error::MissingField:
        return "MissingField";
    case Error::WrongType:
        return "WrongType";
    case Error::TooManyEntries:
        return "TooManyEntries";
    case Error::UnknownMessage:
        return "UnknownMessage";
    case Error::ShortArray:
        return "ShortArray";
    }
    return "Unknown";
}

namespace {

constexpr std::size_t k_initial_scratch = 64 * 1024;

// a ceiling far above the largest real response
constexpr std::size_t k_max_frame = 64 * 1024 * 1024;

[[nodiscard]] constexpr bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// plain pointer to skip the thread_local guard
thread_local const Session* t_current = nullptr;

} // namespace

Error map_error(simdjson::error_code ec) noexcept {
    switch (ec) {
    case simdjson::NO_SUCH_FIELD:
        return Error::MissingField;
    case simdjson::INCORRECT_TYPE:
    case simdjson::NUMBER_ERROR:
    case simdjson::BIGINT_ERROR:
    case simdjson::NUMBER_OUT_OF_RANGE:
    case simdjson::SCALAR_DOCUMENT_AS_VALUE:
        return Error::WrongType;
    default:
        return Error::NotJson;
    }
}

std::expected<void, Error> Session::start(std::string_view frame) {
    if (frame.empty() || frame.size() > k_max_frame) {
        return std::unexpected(Error::NotJson);
    }

    if (scratch_.length() < frame.size()) {
        std::size_t want = k_initial_scratch;
        while (want < frame.size()) {
            want *= 2;
        }
        simdjson::padded_string grown{want};
        if (grown.data() == nullptr) {
            return std::unexpected(Error::NotJson);
        }
        scratch_ = std::move(grown);
    }

    std::memcpy(scratch_.data(), frame.data(), frame.size());
    std::memset(scratch_.data() + frame.size(), 0, simdjson::SIMDJSON_PADDING);

    t_current = this;
    origin_ = frame.data();
    origin_size_ = frame.size();

    const simdjson::padded_string_view view{
        scratch_.data(), frame.size(), frame.size() + simdjson::SIMDJSON_PADDING};
    if (const auto ec = parser_.iterate(view).get(doc_); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return {};
}

std::expected<ondemand::object, Error> Session::open_object(std::string_view frame) {
    if (const auto opened = start(frame); !opened.has_value()) {
        return std::unexpected(opened.error());
    }
    ondemand::object root;
    if (const auto ec = doc_.get_object().get(root); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return root;
}

std::expected<ondemand::array, Error> Session::open_array(std::string_view frame) {
    if (const auto opened = start(frame); !opened.has_value()) {
        return std::unexpected(opened.error());
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

// quotes come off after trimming; contents stay escaped so views can be rebased
std::expected<std::string_view, Error> str_of(ondemand::value value, const Session& s) {
    if (!s.alive()) {
        return std::unexpected(Error::NotJson);
    }
    std::string_view raw;
    if (const auto ec = value.raw_json().get(raw); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    raw = trim(raw);
    if (raw == "null") {
        return std::string_view{};
    }
    if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') {
        return std::unexpected(Error::WrongType);
    }
    return s.rebase(raw.substr(1, raw.size() - 2));
}

std::expected<std::string_view, Error> raw_of(ondemand::value value, const Session& s) {
    if (!s.alive()) {
        return std::unexpected(Error::NotJson);
    }
    std::string_view raw;
    if (const auto ec = value.raw_json().get(raw); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return s.rebase(trim(raw));
}

std::expected<std::uint64_t, Error> u64_of(ondemand::value value) {
    std::uint64_t out = 0;
    if (const auto ec = value.get_uint64().get(out); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return out;
}

std::expected<std::int64_t, Error> i64_of(ondemand::value value) {
    std::int64_t out = 0;
    if (const auto ec = value.get_int64().get(out); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return out;
}

std::expected<bool, Error> bool_of(ondemand::value value) {
    bool out = false;
    if (const auto ec = value.get_bool().get(out); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return out;
}

simdjson::simdjson_result<ondemand::value> find(ondemand::object& obj, std::string_view key) {
    const Session* const s = t_current;
    if (s == nullptr || !s->alive()) {
        return simdjson::TAPE_ERROR;
    }
    const char* const from = s->location();
    auto found = obj.find_field(key);
    if (found.error() != simdjson::NO_SUCH_FIELD || !s->alive()) {
        return found;
    }
    bool has_fields = false;
    if (const auto ec = obj.reset().get(has_fields); ec != simdjson::SUCCESS) {
        return ec;
    }
    for (auto entry : obj) {
        ondemand::field field;
        if (const auto ec = std::move(entry).get(field); ec != simdjson::SUCCESS) {
            return ec;
        }
        if (from != nullptr && field.key().raw() > from) {
            break;
        }
        if (field.key().unsafe_is_equal(key)) {
            return std::move(field).value();
        }
    }
    return simdjson::NO_SUCH_FIELD;
}

std::expected<std::string_view, Error> opt_str(ondemand::object& obj, std::string_view key) {
    ondemand::value value;
    const auto ec = find(obj, key).get(value);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::string_view{};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return str_of(value);
}

std::expected<std::string_view, Error> req_str(ondemand::object& obj, std::string_view key) {
    ondemand::value value;
    if (const auto ec = find(obj, key).get(value); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    auto text = str_of(value);
    if (text && text->empty()) {
        return std::unexpected(Error::MissingField);
    }
    return text;
}

std::expected<std::uint64_t, Error> opt_u64(ondemand::object& obj, std::string_view key) {
    std::uint64_t out = 0;
    const auto ec = find(obj, key).get_uint64().get(out);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::uint64_t{0};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return out;
}

std::expected<std::int64_t, Error> opt_i64(ondemand::object& obj, std::string_view key) {
    std::int64_t out = 0;
    const auto ec = find(obj, key).get_int64().get(out);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::int64_t{0};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return out;
}

std::expected<bool, Error> opt_bool(ondemand::object& obj, std::string_view key) {
    bool out = false;
    const auto ec = find(obj, key).get_bool().get(out);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return false;
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return out;
}

std::expected<std::string_view, Error> opt_raw(ondemand::object& obj, std::string_view key) {
    ondemand::value value;
    const auto ec = find(obj, key).get(value);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::string_view{};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return raw_of(value);
}

std::expected<ondemand::array, Error> req_array(ondemand::object& obj, std::string_view key) {
    ondemand::array out;
    if (const auto ec = find(obj, key).get_array().get(out); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return out;
}

std::expected<ondemand::object, Error> req_object(ondemand::object& obj, std::string_view key) {
    ondemand::object out;
    if (const auto ec = find(obj, key).get_object().get(out); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return out;
}

std::expected<std::uint64_t, Error> opt_ms(ondemand::object& obj, std::string_view key) {
    const auto text = opt_str(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return digits_to_u64(*text);
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

bool has_field(ondemand::object& obj, std::string_view key) {
    ondemand::value value;
    return find(obj, key).get(value) == simdjson::SUCCESS;
}

} // namespace raikiri::wire
