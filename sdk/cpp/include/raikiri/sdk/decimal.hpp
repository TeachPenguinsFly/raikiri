#pragma once

#include <compare>
#include <concepts>
#include <cstdint>
#include <expected>
#include <stdexcept>
#include <string>
#include <string_view>

namespace raikiri::sdk {

// An exact decimal with nine places, held as a 64-bit count of billionths, so about plus or minus 9.2 billion.
// There is no way in from a double. Arithmetic that overflows throws std::overflow_error.
class Decimal {
public:
    constexpr Decimal() noexcept = default;

    template <std::integral I>
    constexpr Decimal(I whole) : raw_{scaled(static_cast<std::int64_t>(whole))} {} // NOLINT: an int is exact

    Decimal(float) = delete;
    Decimal(double) = delete;
    Decimal(long double) = delete;

    [[nodiscard]] static constexpr Decimal from_raw(std::int64_t raw) noexcept {
        Decimal out;
        out.raw_ = raw;
        return out;
    }

    // refuses more than nine fractional digits rather than rounding
    [[nodiscard]] static std::expected<Decimal, std::string> parse(std::string_view text);

    [[nodiscard]] constexpr std::int64_t raw() const noexcept { return raw_; }

    // the shortest exact text: "68000.5", "0.001", "-2"
    [[nodiscard]] std::string str() const;

    friend constexpr auto operator<=>(Decimal, Decimal) noexcept = default;

    [[nodiscard]] friend constexpr Decimal operator+(Decimal a, Decimal b) {
        std::int64_t out{};
        if (__builtin_add_overflow(a.raw_, b.raw_, &out)) {
            throw std::overflow_error{"Decimal addition overflow"};
        }
        return from_raw(out);
    }

    [[nodiscard]] friend constexpr Decimal operator-(Decimal a, Decimal b) {
        std::int64_t out{};
        if (__builtin_sub_overflow(a.raw_, b.raw_, &out)) {
            throw std::overflow_error{"Decimal subtraction overflow"};
        }
        return from_raw(out);
    }

    [[nodiscard]] friend constexpr Decimal operator-(Decimal a) { return Decimal{} - a; }

    // rounds toward zero at the ninth place
    friend Decimal operator*(Decimal a, Decimal b);
    friend Decimal operator/(Decimal a, Decimal b);

    constexpr Decimal& operator+=(Decimal b) { return *this = *this + b; }

    constexpr Decimal& operator-=(Decimal b) { return *this = *this - b; }

    // down to a multiple of `step`, toward minus infinity: a price onto its tick, a size onto its lot
    [[nodiscard]] constexpr Decimal floor_to(Decimal step) const {
        if (step.raw_ <= 0) {
            throw std::invalid_argument{"Decimal step must be positive"};
        }
        auto count = raw_ / step.raw_;
        if (raw_ < 0 && raw_ % step.raw_ != 0) {
            --count;
        }
        std::int64_t out{};
        if (__builtin_mul_overflow(count, step.raw_, &out)) {
            throw std::overflow_error{"Decimal floor overflow"};
        }
        return from_raw(out);
    }

    [[nodiscard]] constexpr Decimal abs() const { return raw_ < 0 ? -*this : *this; }

private:
    static constexpr std::int64_t k_scale = 1'000'000'000;

    static constexpr std::int64_t scaled(std::int64_t whole) {
        std::int64_t out{};
        if (__builtin_mul_overflow(whole, k_scale, &out)) {
            throw std::overflow_error{"Decimal out of range"};
        }
        return out;
    }

    std::int64_t raw_{0};
};

namespace internal {

consteval std::int64_t parse_literal(std::string_view text) {
    bool negative = false;
    if (!text.empty() && text.front() == '-') {
        negative = true;
        text.remove_prefix(1);
    }
    if (text.empty()) {
        throw "empty decimal literal";
    }
    std::int64_t value = 0;
    int places = -1;
    for (const char c : text) {
        if (c == '.') {
            if (places >= 0) {
                throw "two dots in a decimal literal";
            }
            places = 0;
            continue;
        }
        if (c < '0' || c > '9') {
            throw "not a decimal literal";
        }
        if (places >= 0 && ++places > 9) {
            throw "more than nine decimal places";
        }
        value = value * 10 + (c - '0');
    }
    for (int i = places < 0 ? 0 : places; i < 9; ++i) {
        value *= 10;
    }
    return negative ? -value : value;
}

} // namespace internal

inline namespace literals {

// "68000.5"_d, checked at compile time
consteval Decimal operator""_d(const char* text, std::size_t size) {
    return Decimal::from_raw(internal::parse_literal({text, size}));
}

} // namespace literals

} // namespace raikiri::sdk
