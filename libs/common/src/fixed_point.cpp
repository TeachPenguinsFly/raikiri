#include <raikiri/common/fixed_point.hpp>

#include <charconv>
#include <cstring>

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
                // More precision than the model holds. Silent rounding here
                // would make two processes disagree about the same wire value.
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
    std::uint64_t mag = raw < 0 ? static_cast<std::uint64_t>(-(raw + 1)) + 1U
                                : static_cast<std::uint64_t>(raw);
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

} // namespace raikiri
