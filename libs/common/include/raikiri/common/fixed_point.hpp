#pragma once

// Fixed-point money & rate types.
//
// All decision math in this codebase is integer fixed-point. `double` never
// touches prices, quantities, or rates: two venues with different tick sizes
// round binary floats differently, and that drift is a class of real-money
// bugs. Decimal <-> integer conversion happens only at the process edges
// (wire parse / wire serialize), through the functions in this header.
//
// Arithmetic aborts on overflow (crash-only philosophy: corrupt data is worse
// than no data). The branch is one predictable compare on the hot path.

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

// Strongly-typed int64 with an explicit scale meaning per tag. No implicit
// conversions between tags; no implicit construction from raw integers except
// via the named factory, so call sites read as what they are.
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

// 128-bit value for notionals and other i64*i64 products. Comparable POD;
// never serialized to the wire (recomputed where needed).
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

// a*b/d with the product held in 128 bits and narrowed once, truncating toward
// zero. Aborts on a zero divisor or a quotient that does not fit i64.
//
// The intermediate is what needs the width, not the answer: 0.5 BTC at 50 000
// is 5e8 * 5e13 = 2.5e22 in e9 operands, past i64, while the result 2.5e13 has
// six decades of headroom. Folding the scale factors into one precomputed
// multiplier instead is the trap it exists to avoid — for a sub-unit contract
// value that multiplier truncates to zero and every product with it is
// silently zero rather than loudly wrong.
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

enum class ParseError : std::uint8_t {
    Empty,
    BadChar,
    OutOfRange,
    TooManyFracDigits,
};

// Parse a decimal string ("0.0001", "-12.5", "3") into 1e9 fixed-point.
// Edge-only: wire parsing and config loading. Rejects >9 significant
// fractional digits rather than silently rounding — a venue sending more
// precision than we model is a schema event, not data.
[[nodiscard]] std::expected<FixedE9, ParseError> parse_decimal_e9(std::string_view text) noexcept;

// Format 1e9 fixed-point back into a decimal string without trailing zeros.
// `buf` must hold >= 32 bytes; returns the view into buf.
[[nodiscard]] std::string_view format_e9(FixedE9 value, char* buf, std::size_t buf_len) noexcept;

// Exponent-aware decimal reading.
//
// parse_decimal_e9 above reads plain decimals in int64 and is the hot-path
// reader. It cannot read scientific notation, and venues spell numbers however
// their serialiser feels: HTX ships `1.000000000E-9` and `0.100000000000000000`
// in ONE response, BitMEX writes `0.0001` over REST and `1.0E-4` over the
// socket for the same field, Deribit sends strike prices as `6.4e4`. Six venue
// adapters independently wrote the same int128-mantissa-plus-exponent reader
// before it was worth admitting it belongs here.
//
// The two entry points differ ONLY in what happens to significance below 1e-9,
// and that difference is the reason there are two:
//
//   parse_number_e9 REFUSES it. Prices and quantities must be exact — a value
//   silently rounded is an order one tick from where it was meant.
//
//   parse_rate_e9 TRUNCATES toward zero. Funding rates arrive padded to 18
//   fractional digits with a non-zero tail (`-0.000058784590419109` live on
//   HTX), so an exact read refuses every frame the venue has ever sent and
//   presents a running stream as silence. Truncating a dimensionless rate at
//   1e-9 leaves eight significant figures on a number nothing divides an order
//   by. Never use this for a price or a size.
[[nodiscard]] std::optional<FixedE9> parse_number_e9(std::string_view text) noexcept;
[[nodiscard]] std::optional<FixedE9> parse_rate_e9(std::string_view text) noexcept;

} // namespace raikiri
