#include <raikiri/audit/line.hpp>

#include <array>
#include <charconv>
#include <glaze/glaze.hpp>

namespace raikiri::audit {

namespace internal {

constexpr std::string_view k_prev_key = ",\"prev\":\"";
constexpr std::string_view k_tail = "\"}";
constexpr std::size_t k_hex_chars = 64;

[[nodiscard]] std::size_t utf8_length(std::string_view text, std::size_t at) noexcept {
    const auto lead = static_cast<unsigned char>(text[at]);
    std::size_t length = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
    } else {
        return 0;
    }
    if (at + length > text.size()) {
        return 0;
    }
    for (std::size_t i = 1; i < length; ++i) {
        if ((static_cast<unsigned char>(text[at + i]) & 0xC0U) != 0x80U) {
            return 0;
        }
    }
    const auto second = static_cast<unsigned char>(text[at + 1]);
    if ((lead == 0xE0 && second < 0xA0) || (lead == 0xED && second > 0x9F) || (lead == 0xF0 && second < 0x90) ||
        (lead == 0xF4 && second > 0x8F)) {
        return 0;
    }
    return length;
}

// the file must stay parseable whatever a caller passes, so a bad byte becomes U+FFFD
void append_escaped(std::string& out, std::string_view text) {
    constexpr std::string_view k_digits = "0123456789abcdef";
    out.push_back('"');
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
            ++i;
        } else if (c < 0x20) {
            out.append("\\u00");
            out.push_back(k_digits[c >> 4U]);
            out.push_back(k_digits[c & 0x0FU]);
            ++i;
        } else if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
        } else if (const auto length = utf8_length(text, i); length != 0) {
            out.append(text.substr(i, length));
            i += length;
        } else {
            out.append("\\ufffd");
            ++i;
        }
    }
    out.push_back('"');
}

[[nodiscard]] bool detail_is_json(std::string_view detail) noexcept {
    return detail.find('\n') == std::string_view::npos && !glz::validate_json(detail);
}

class Cursor {
public:
    explicit Cursor(std::string_view text) noexcept : rest_{text} {}

    [[nodiscard]] bool literal(std::string_view expected) noexcept {
        if (!rest_.starts_with(expected)) {
            return false;
        }
        rest_.remove_prefix(expected.size());
        return true;
    }

    [[nodiscard]] bool digits(std::uint64_t& value) noexcept {
        const auto [stop, ec] = std::from_chars(rest_.data(), rest_.data() + rest_.size(), value);
        if (ec != std::errc{}) {
            return false;
        }
        rest_.remove_prefix(static_cast<std::size_t>(stop - rest_.data()));
        return true;
    }

    // past the closing quote, with the string left escaped
    [[nodiscard]] bool string(std::string_view& value) noexcept {
        for (std::size_t i = 0; i < rest_.size(); ++i) {
            if (rest_[i] == '\\') {
                ++i;
            } else if (rest_[i] == '"') {
                value = rest_.substr(0, i);
                rest_.remove_prefix(i + 1);
                return true;
            }
        }
        return false;
    }

private:
    std::string_view rest_;
};

void append_code_point(std::string& out, std::uint32_t code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0U | (code >> 6U)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xE0U | (code >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    }
}

} // namespace internal

bool append_line(std::string& out, std::uint64_t seq, const Entry& entry, const Digest& prev) {
    std::array<char, 24> digits{};
    const auto number = [&digits](std::uint64_t value) {
        const auto [stop, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), value);
        (void)ec;
        return std::string_view{digits.data(), static_cast<std::size_t>(stop - digits.data())};
    };
    out.append("{\"seq\":");
    out.append(number(seq));
    out.append(",\"ts\":\"");
    out.append(number(entry.ts.ns));
    out.append("\",\"actor\":");
    internal::append_escaped(out, entry.actor);
    out.append(",\"action\":");
    internal::append_escaped(out, entry.action);
    out.append(",\"subject\":");
    internal::append_escaped(out, entry.subject);
    out.append(",\"detail\":");
    bool as_given = true;
    if (entry.detail_json.empty()) {
        out.append("{}");
    } else if (internal::detail_is_json(entry.detail_json)) {
        out.append(entry.detail_json);
    } else {
        as_given = false;
        out.append("{\"unparsed\":");
        internal::append_escaped(out, entry.detail_json);
        out.push_back('}');
    }
    out.append(internal::k_prev_key);
    out.append(to_hex(prev));
    out.append(internal::k_tail);
    return as_given;
}

std::optional<ParsedLine> parse_line(std::string_view line) noexcept {
    const std::size_t tail_bytes = internal::k_prev_key.size() + internal::k_hex_chars + internal::k_tail.size();
    if (line.size() < tail_bytes || !line.ends_with(internal::k_tail)) {
        return std::nullopt;
    }
    const auto tail = line.substr(line.size() - tail_bytes);
    if (!tail.starts_with(internal::k_prev_key)) {
        return std::nullopt;
    }
    const auto prev = digest_from_hex(tail.substr(internal::k_prev_key.size(), internal::k_hex_chars));
    if (!prev.has_value()) {
        return std::nullopt;
    }
    ParsedLine out{};
    out.prev = *prev;
    internal::Cursor cursor{line.substr(0, line.size() - tail_bytes)};
    constexpr std::string_view k_detail_key = ",\"detail\":";
    if (!cursor.literal("{\"seq\":") || !cursor.digits(out.seq) || !cursor.literal(",\"ts\":\"") ||
        !cursor.digits(out.ts) || !cursor.literal("\",\"actor\":\"") || !cursor.string(out.actor) ||
        !cursor.literal(",\"action\":\"") || !cursor.string(out.action) || !cursor.literal(",\"subject\":\"") ||
        !cursor.string(out.subject) || !cursor.literal(k_detail_key)) {
        return std::nullopt;
    }
    const std::size_t detail_at = static_cast<std::size_t>(out.subject.data() - line.data()) + out.subject.size() + 1 +
                                  k_detail_key.size();
    out.detail = line.substr(detail_at, line.size() - tail_bytes - detail_at);
    if (out.detail.empty()) {
        return std::nullopt;
    }
    return out;
}

void unescape_to(std::string& out, std::string_view escaped) {
    out.clear();
    if (escaped.find('\\') == std::string_view::npos) {
        out.assign(escaped);
        return;
    }
    for (std::size_t i = 0; i < escaped.size(); ++i) {
        const char c = escaped[i];
        if (c != '\\' || i + 1 == escaped.size()) {
            out.push_back(c);
            continue;
        }
        const char next = escaped[++i];
        switch (next) {
        case 'n':
            out.push_back('\n');
            break;
        case 't':
            out.push_back('\t');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 'b':
            out.push_back('\b');
            break;
        case 'f':
            out.push_back('\f');
            break;
        case 'u': {
            std::uint32_t code = 0xFFFD;
            if (i + 4 < escaped.size()) {
                const auto [stop, ec] = std::from_chars(escaped.data() + i + 1, escaped.data() + i + 5, code, 16);
                if (ec != std::errc{} || stop != escaped.data() + i + 5) {
                    code = 0xFFFD;
                }
                i += 4;
            }
            internal::append_code_point(out, code);
            break;
        }
        default:
            out.push_back(next);
        }
    }
}

} // namespace raikiri::audit
