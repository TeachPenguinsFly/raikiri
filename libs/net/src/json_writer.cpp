#include <raikiri/net/json_writer.hpp>

#include <charconv>
#include <cstring>

namespace raikiri::net {

void JsonWriter::put(char c) noexcept {
    if (overflow_ || len_ + 1 > buf_.size()) {
        overflow_ = true;
        return;
    }
    buf_[len_++] = c;
}

void JsonWriter::put(std::string_view s) noexcept {
    if (overflow_ || len_ + s.size() > buf_.size()) {
        overflow_ = true;
        return;
    }
    s.copy(buf_.data() + len_, s.size());
    len_ += s.size();
}

// escaping rarely fires; venue symbols and ulids are alphanumeric
void JsonWriter::put_escaped(std::string_view s) noexcept {
    for (const char c : s) {
        switch (c) {
        case '"':
            put("\\\"");
            break;
        case '\\':
            put("\\\\");
            break;
        case '\n':
            put("\\n");
            break;
        case '\r':
            put("\\r");
            break;
        case '\t':
            put("\\t");
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char esc[7];
                std::snprintf(esc, sizeof esc, "\\u%04x", static_cast<unsigned>(c));
                put(std::string_view{esc, 6});
            } else {
                put(c);
            }
        }
    }
}

void JsonWriter::separator() noexcept {
    if (need_comma_) {
        put(',');
    }
    need_comma_ = true;
}

void JsonWriter::put_key(std::string_view key) noexcept {
    separator();
    put('"');
    put_escaped(key);
    put("\":");
}

JsonWriter& JsonWriter::obj_begin() noexcept {
    separator();
    put('{');
    need_comma_ = false;
    return *this;
}

JsonWriter& JsonWriter::obj_end() noexcept {
    put('}');
    need_comma_ = true;
    return *this;
}

JsonWriter& JsonWriter::arr_begin() noexcept {
    separator();
    put('[');
    need_comma_ = false;
    return *this;
}

JsonWriter& JsonWriter::arr_end() noexcept {
    put(']');
    need_comma_ = true;
    return *this;
}

JsonWriter& JsonWriter::str(std::string_view key, std::string_view value) noexcept {
    put_key(key);
    put('"');
    put_escaped(value);
    put('"');
    return *this;
}

JsonWriter& JsonWriter::num(std::string_view key, std::int64_t value) noexcept {
    put_key(key);
    char tmp[24];
    const auto [end, ec] = std::to_chars(tmp, tmp + sizeof tmp, value);
    if (ec != std::errc{}) {
        overflow_ = true;
        return *this;
    }
    put(std::string_view{tmp, static_cast<std::size_t>(end - tmp)});
    return *this;
}

JsonWriter& JsonWriter::boolean(std::string_view key, bool value) noexcept {
    put_key(key);
    put(value ? "true" : "false");
    return *this;
}

JsonWriter& JsonWriter::decimal(std::string_view key, FixedE9 value) noexcept {
    put_key(key);
    char tmp[32];
    const auto text = format_e9(value, tmp, sizeof tmp);
    if (text.empty()) {
        overflow_ = true; // an undefined value reaching the wire is a bug
        return *this;
    }
    put('"');
    put(text);
    put('"');
    return *this;
}

JsonWriter& JsonWriter::decimal_raw(std::string_view key, FixedE9 value) noexcept {
    put_key(key);
    char tmp[32];
    const auto text = format_e9(value, tmp, sizeof tmp);
    if (text.empty()) {
        overflow_ = true;
        return *this;
    }
    put(text);
    return *this;
}

JsonWriter& JsonWriter::raw(std::string_view key, std::string_view json) noexcept {
    put_key(key);
    put(json);
    return *this;
}

} // namespace raikiri::net
