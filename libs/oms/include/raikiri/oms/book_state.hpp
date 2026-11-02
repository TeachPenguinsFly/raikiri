#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace raikiri::oms {

// a book's permission: reducing lets only orders that shrink its position through, halted only cancels
enum class BookState : std::uint8_t { Active, Reducing, Halted };

[[nodiscard]] constexpr std::string_view to_string(BookState state) noexcept {
    switch (state) {
    case BookState::Active:
        return "active";
    case BookState::Reducing:
        return "reducing";
    case BookState::Halted:
        return "halted";
    }
    return "?";
}

[[nodiscard]] constexpr std::optional<BookState> book_state_from(std::string_view text) noexcept {
    for (const auto state : {BookState::Active, BookState::Reducing, BookState::Halted}) {
        if (to_string(state) == text) {
            return state;
        }
    }
    return std::nullopt;
}

[[nodiscard]] constexpr BookState stricter(BookState a, BookState b) noexcept {
    return a > b ? a : b;
}

} // namespace raikiri::oms
