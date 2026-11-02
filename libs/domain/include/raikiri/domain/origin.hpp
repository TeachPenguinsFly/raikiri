#pragma once

// whose an order is and why it exists; the book's id is the strategy half of its ClientOrderId

#include <raikiri/common/fixed_string.hpp>

#include <cstdint>
#include <string_view>

namespace raikiri {

enum class BookKind : std::uint8_t { Unknown = 0, Strategy, Manual, Client, Hedge };

[[nodiscard]] constexpr std::string_view to_string(BookKind kind) noexcept {
    switch (kind) {
    case BookKind::Strategy:
        return "strategy";
    case BookKind::Manual:
        return "manual";
    case BookKind::Client:
        return "client";
    case BookKind::Hedge:
        return "hedge";
    case BookKind::Unknown:
        break;
    }
    return "unknown";
}

[[nodiscard]] constexpr BookKind book_kind_from_string(std::string_view text) noexcept {
    for (const auto kind : {BookKind::Strategy, BookKind::Manual, BookKind::Client, BookKind::Hedge}) {
        if (to_string(kind) == text) {
            return kind;
        }
    }
    return BookKind::Unknown;
}

// an rfq trade id or a client's own order id; as long as the journal's venueText
inline constexpr std::size_t k_external_ref_len = 40;
using ExternalRef = FixedString<k_external_ref_len>;

struct OrderOrigin {
    std::uint64_t parent{0}; // zero is no parent
    ExternalRef external_ref{};
};

} // namespace raikiri
