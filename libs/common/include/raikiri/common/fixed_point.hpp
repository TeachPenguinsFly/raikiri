#pragma once

// decision math is integer fixed-point; decimals convert only at the process edges

#include <raikiri/common/int128.hpp>

#include <compare>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <limits>
#include <optional>
#include <string_view>

namespace raikiri {

inline constexpr std::int64_t k_undef_raw = std::numeric_limits<std::int64_t>::min();

namespace detail {

[[nodiscard]] inline std::int64_t checked_add(std::int64_t a, std::int64_t b) noexcept {
    std::int64_t out{};
    if (__builtin_add_overflow(a, b, &out)) [[unlikely]] {
        std::abort();
    }
    return out;
}

[[nodiscard]] inline std::int64_t checked_sub(std::int64_t a, std::int64_t b) noexcept {
    std::int64_t out{};
    if (__builtin_sub_overflow(a, b, &out)) [[unlikely]] {
        std::abort();
    }
    return out;
}

[[nodiscard]] inline std::int64_t checked_mul(std::int64_t a, std::int64_t b) noexcept {
    std::int64_t out{};
    if (__builtin_mul_overflow(a, b, &out)) [[unlikely]] {
        std::abort();
    }
    return out;
}

} // namespace detail

template <typename Tag> struct Scaled64 {
    std::int64_t raw{k_undef_raw};

    [[nodiscard]] static constexpr Scaled64 from_raw(std::int64_t r) noexcept { return {r}; }

    [[nodiscard]] static constexpr Scaled64 undef() noexcept { return {k_undef_raw}; }

    [[nodiscard]] constexpr bool is_undef() const noexcept { return raw == k_undef_raw; }

    friend constexpr auto operator<=>(Scaled64, Scaled64) noexcept = default;

    [[nodiscard]] friend Scaled64 operator+(Scaled64 a, Scaled64 b) noexcept {
        return {detail::checked_add(a.raw, b.raw)};
    }

    [[nodiscard]] friend Scaled64 operator-(Scaled64 a, Scaled64 b) noexcept {
        return {detail::checked_sub(a.raw, b.raw)};
    }

    [[nodiscard]] Scaled64 scaled_by(std::int64_t n) const noexcept {
        return {detail::checked_mul(raw, n)};
    }
};

struct PriceTag {};

struct QtyTag {};

struct E9Tag {};

// Price in integer multiples of the instrument's tick size.
using PriceTicks = Scaled64<PriceTag>;

// Quantity in integer multiples of the instrument's lot size.
using QtyLots = Scaled64<QtyTag>;

// Decimal scaled by 1e9: funding rates, fee rates, small ratios.
using FixedE9 = Scaled64<E9Tag>;

inline constexpr std::int64_t k_e9 = 1'000'000'000;

struct E6Tag {};

// 1e6 because a busy contract's daily quote turnover passes i64 at 1e9
using FixedE6 = Scaled64<E6Tag>;

inline constexpr std::int64_t k_e6 = 1'000'000;

struct Notional128 {
    int128_t raw{0};

    friend constexpr auto operator<=>(Notional128, Notional128) noexcept = default;

    [[nodiscard]] friend constexpr Notional128 operator+(Notional128 a, Notional128 b) noexcept {
        return {a.raw + b.raw};
    }
};

[[nodiscard]] constexpr Notional128 mul_wide(std::int64_t a, std::int64_t b) noexcept {
    return {static_cast<int128_t>(a) * static_cast<int128_t>(b)};
}

// a*b/d through a 128-bit product, truncating toward zero; aborts on a zero divisor or i64 overflow
[[nodiscard]] constexpr std::int64_t
mul_div(std::int64_t a, std::int64_t b, std::int64_t d) noexcept {
    if (d == 0) [[unlikely]] {
        std::abort();
    }
    const int128_t quotient = (static_cast<int128_t>(a) * static_cast<int128_t>(b)) / d;
    if (quotient > static_cast<int128_t>(std::numeric_limits<std::int64_t>::max()) ||
        quotient < static_cast<int128_t>(std::numeric_limits<std::int64_t>::min())) [[unlikely]] {
        std::abort();
    }
    return static_cast<std::int64_t>(quotient);
}

// undef rather than an abort past i64, since both operands come from the venue
[[nodiscard]] constexpr FixedE9 turnover_e9(FixedE9 qty, FixedE9 price) noexcept {
    if (qty.is_undef() || price.is_undef()) {
        return FixedE9::undef();
    }
    const int128_t product = (static_cast<int128_t>(qty.raw) * price.raw) / k_e9;
    if (product > std::numeric_limits<std::int64_t>::max() ||
        product <= std::numeric_limits<std::int64_t>::min()) {
        return FixedE9::undef();
    }
    return FixedE9::from_raw(static_cast<std::int64_t>(product));
}

[[nodiscard]] constexpr FixedE6 turnover_e6(FixedE9 qty, FixedE9 price) noexcept {
    if (qty.is_undef() || price.is_undef()) {
        return FixedE6::undef();
    }
    const int128_t product = (static_cast<int128_t>(qty.raw) * price.raw) / (k_e9 * (k_e9 / k_e6));
    if (product > std::numeric_limits<std::int64_t>::max() ||
        product <= std::numeric_limits<std::int64_t>::min()) {
        return FixedE6::undef();
    }
    return FixedE6::from_raw(static_cast<std::int64_t>(product));
}

enum class ParseError : std::uint8_t {
    Empty,
    BadChar,
    OutOfRange,
    TooManyFracDigits,
};

// rejects more than 9 fractional digits rather than rounding
[[nodiscard]] std::expected<FixedE9, ParseError> parse_decimal_e9(std::string_view text) noexcept;

// buf must hold at least 32 bytes
[[nodiscard]] std::string_view format_e9(FixedE9 value, char* buf, std::size_t buf_len) noexcept;

// reads exponent notation too; refuses significance below 1e-9, which parse_rate_e9 truncates
// instead
[[nodiscard]] std::optional<FixedE9> parse_number_e9(std::string_view text) noexcept;
[[nodiscard]] std::optional<FixedE9> parse_rate_e9(std::string_view text) noexcept;

// truncates below 1e-9 so a balance sent at finer resolution still reads; not for a price or a size
[[nodiscard]] std::optional<FixedE9> parse_amount_e9(std::string_view text) noexcept;

[[nodiscard]] std::optional<FixedE6> parse_turnover_e6(std::string_view text) noexcept;

} // namespace raikiri
