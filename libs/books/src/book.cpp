#include <raikiri/books/book.hpp>

#include <algorithm>

namespace raikiri::books {

std::string_view to_string(BookError error) noexcept {
    switch (error) {
    case BookError::BadName:
        return "a book is manual, manual:<login>, client:<name> or hedge; a label is 1 to 64 of a-z A-Z 0-9 . _ @ -";
    case BookError::NotFound:
        return "no such book";
    case BookError::IdTaken:
        return "this strategy id is on record under another name, and ids are never reused";
    case BookError::Exhausted:
        return "every book id has been handed out";
    case BookError::Store:
        return "the book registry would not read or write";
    }
    return "refused";
}

bool valid_label(std::string_view label) noexcept {
    return !label.empty() && label.size() <= 64 && std::ranges::all_of(label, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
               c == '@' || c == '-';
    });
}

BookKind reserved_kind(std::string_view name) noexcept {
    if (name == k_shared_manual) {
        return BookKind::Manual;
    }
    if (name == k_hedge) {
        return BookKind::Hedge;
    }
    if (name.starts_with(k_manual_prefix) && valid_label(name.substr(k_manual_prefix.size()))) {
        return BookKind::Manual;
    }
    if (name.starts_with(k_client_prefix) && valid_label(name.substr(k_client_prefix.size()))) {
        return BookKind::Client;
    }
    return BookKind::Unknown;
}

std::string manual_book_name(std::string_view login) {
    if (!valid_label(login)) {
        return std::string{k_shared_manual};
    }
    std::string out{k_manual_prefix};
    out += login;
    return out;
}

std::expected<std::uint16_t, BookError> next_id(std::span<const Book> on_record) noexcept {
    std::uint32_t highest = k_first_allocated - 1U;
    for (const auto& book : on_record) {
        // a strategy parked above the range must not end it
        if (book.id <= k_last_allocated) {
            highest = std::max<std::uint32_t>(highest, book.id);
        }
    }
    if (highest >= k_last_allocated) {
        return std::unexpected{BookError::Exhausted};
    }
    return static_cast<std::uint16_t>(highest + 1);
}

} // namespace raikiri::books
