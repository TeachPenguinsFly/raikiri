#pragma once

// inline storage for venue-minted ids, which must not be interned

#include <array>
#include <compare>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <string_view>

namespace raikiri {

enum class StrError : std::uint8_t { TooLong };

template <std::size_t N> struct FixedString {
    static_assert(N > 0 && N <= 255, "length byte is uint8_t");

    std::array<char, N> data{};
    std::uint8_t len{0};

    [[nodiscard]] static std::expected<FixedString, StrError> from(std::string_view sv) noexcept {
        if (sv.size() > N) {
            return std::unexpected{StrError::TooLong};
        }
        FixedString out{};
        // an empty view may hold a null pointer, which memcpy may not be handed even for zero bytes
        if (!sv.empty()) {
            std::memcpy(out.data.data(), sv.data(), sv.size());
        }
        out.len = static_cast<std::uint8_t>(sv.size());
        return out;
    }

    [[nodiscard]] std::string_view view() const noexcept { return {data.data(), len}; }

    [[nodiscard]] bool empty() const noexcept { return len == 0; }

    [[nodiscard]] friend bool operator==(const FixedString& a, const FixedString& b) noexcept {
        return a.view() == b.view();
    }

    [[nodiscard]] friend auto operator<=>(const FixedString& a, const FixedString& b) noexcept {
        return a.view() <=> b.view();
    }
};

} // namespace raikiri

template <std::size_t N> struct std::hash<raikiri::FixedString<N>> {
    [[nodiscard]] std::size_t operator()(const raikiri::FixedString<N>& s) const noexcept {
        return std::hash<std::string_view>{}(s.view());
    }
};
