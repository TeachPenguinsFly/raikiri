#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/int128.hpp>

#include <charconv>
#include <cstring>
#include <limits>
#include <optional>

namespace raikiri {

std::expected<FixedE9, ParseError> parse_decimal_e9(std::string_view text) noexcept {
    if (text.empty()) {
        return std::unexpected{ParseError::Empty};
    }

    const char* p = text.data();
    const char* const end = text.data() + text.size();

    bool negative = false;
    if (*p == '-' || *p == '+') {
        negative = (*p == '-');
        ++p;
    }
    if (p == end) {
        return std::unexpected{ParseError::Empty};
    }

    std::int64_t int_part = 0;
    bool any_digit = false;
    for (; p != end && *p != '.'; ++p) {
        if (*p < '0' || *p > '9') {
            return std::unexpected{ParseError::BadChar};
        }
        any_digit = true;
        std::int64_t scaled{};
        if (__builtin_mul_overflow(int_part, std::int64_t{10}, &scaled) ||
            __builtin_add_overflow(scaled, std::int64_t{*p - '0'}, &int_part)) {
            return std::unexpected{ParseError::OutOfRange};
        }
    }

    std::int64_t frac_e9 = 0;
    if (p != end) { // consume '.'
        ++p;
        int digits = 0;
        for (; p != end; ++p) {
            if (*p < '0' || *p > '9') {
                return std::unexpected{ParseError::BadChar};
            }
            any_digit = true;
            if (digits < 9) {
                frac_e9 = frac_e9 * 10 + (*p - '0');
                ++digits;
            } else if (*p != '0') {
                return std::unexpected{ParseError::TooManyFracDigits};
            }
        }
        for (; digits < 9; ++digits) {
            frac_e9 *= 10;
        }
    }
    if (!any_digit) {
        return std::unexpected{ParseError::Empty};
    }

    std::int64_t out{};
    if (__builtin_mul_overflow(int_part, k_e9, &out) ||
        __builtin_add_overflow(out, frac_e9, &out)) {
        return std::unexpected{ParseError::OutOfRange};
    }
    if (negative) {
        out = -out;
    }
    return FixedE9::from_raw(out);
}

std::string_view format_e9(FixedE9 value, char* buf, std::size_t buf_len) noexcept {
    // Worst case: "-9223372036.854775807" = 21 chars.
    if (buf_len < 32 || value.is_undef()) {
        return {};
    }
    char* p = buf;
    std::int64_t raw = value.raw;
    std::uint64_t mag =
        raw < 0 ? static_cast<std::uint64_t>(-(raw + 1)) + 1U : static_cast<std::uint64_t>(raw);
    if (raw < 0) {
        *p++ = '-';
    }

    const std::uint64_t int_part = mag / static_cast<std::uint64_t>(k_e9);
    std::uint64_t frac = mag % static_cast<std::uint64_t>(k_e9);

    const auto [int_end, ec] = std::to_chars(p, buf + buf_len, int_part);
    p = int_end;

    if (frac != 0) {
        *p++ = '.';
        char frac_digits[9];
        for (int i = 8; i >= 0; --i) {
            frac_digits[i] = static_cast<char>('0' + (frac % 10));
            frac /= 10;
        }
        int keep = 9;
        while (keep > 1 && frac_digits[keep - 1] == '0') {
            --keep;
        }
        std::memcpy(p, frac_digits, static_cast<std::size_t>(keep));
        p += keep;
    }
    return {buf, static_cast<std::size_t>(p - buf)};
}

namespace {

// Bounds the int128 accumulator and the power-of-ten loops. No live token from
// any of the sixteen venues comes near either.
constexpr int k_max_mantissa_digits = 30;
constexpr int k_max_shift = 30;

struct Decomposed {
    int128_t mantissa{0};
    int exponent{0}; // value == mantissa * 10^exponent
    bool negative{false};
};

std::optional<Decomposed> decompose(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    Decomposed out{};
    std::size_t i = 0;
    if (text[i] == '+' || text[i] == '-') {
        out.negative = text[i] == '-';
        ++i;
    }
    int digits = 0;
    int fractional = 0;
    bool seen_digit = false;
    bool seen_dot = false;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '.') {
            if (seen_dot) {
                return std::nullopt;
            }
            seen_dot = true;
            continue;
        }
        if (c == 'e' || c == 'E') {
            break;
        }
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        seen_digit = true;
        if (out.mantissa != 0 || c != '0') {
            if (++digits > k_max_mantissa_digits) {
                return std::nullopt;
            }
            out.mantissa = out.mantissa * 10 + (c - '0');
        }
        if (seen_dot) {
            ++fractional;
        }
    }
    if (!seen_digit) {
        return std::nullopt;
    }
    out.exponent = -fractional;

    if (i < text.size()) {
        ++i; // skip the 'e'
        bool exponent_negative = false;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            exponent_negative = text[i] == '-';
            ++i;
        }
        if (i >= text.size()) {
            return std::nullopt;
        }
        int value = 0;
        for (; i < text.size(); ++i) {
            const char c = text[i];
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            value = value * 10 + (c - '0');
            if (value > k_max_shift * 4) {
                return std::nullopt;
            }
        }
        out.exponent += exponent_negative ? -value : value;
    }
    return out;
}

std::optional<FixedE9> scale_to_e9(const Decomposed& parts, bool exact) noexcept {
    int shift = parts.exponent + 9;
    if (shift > k_max_shift || shift < -k_max_shift) {
        return std::nullopt;
    }
    int128_t value = parts.mantissa;
    for (; shift > 0; --shift) {
        value *= 10;
    }
    for (; shift < 0; ++shift) {
        if (exact && value % 10 != 0) {
            return std::nullopt;
        }
        value /= 10;
    }
    if (parts.negative) {
        value = -value;
    }
    constexpr auto k_i64_max = static_cast<int128_t>(std::numeric_limits<std::int64_t>::max());
    if (value > k_i64_max || value < -k_i64_max) {
        return std::nullopt;
    }
    return FixedE9::from_raw(static_cast<std::int64_t>(value));
}

} // namespace

std::optional<FixedE9> parse_number_e9(std::string_view text) noexcept {
    const auto parts = decompose(text);
    return parts.has_value() ? scale_to_e9(*parts, /*exact=*/true) : std::nullopt;
}

std::optional<FixedE9> parse_amount_e9(std::string_view text) noexcept {
    // Same reader; the difference is the promise at the call site.
    return parse_rate_e9(text);
}

std::optional<FixedE9> parse_rate_e9(std::string_view text) noexcept {
    const auto parts = decompose(text);
    return parts.has_value() ? scale_to_e9(*parts, /*exact=*/false) : std::nullopt;
}

} // namespace raikiri
