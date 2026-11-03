#pragma once

#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/origin.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace raikiri::books {

inline constexpr std::string_view k_shared_manual = "manual";
inline constexpr std::string_view k_hedge = "hedge";
inline constexpr std::string_view k_desk = "desk";
inline constexpr std::string_view k_manual_prefix = "manual:";
inline constexpr std::string_view k_client_prefix = "client:";

// below this a person picks the number for a strategy; above it the registry hands them out
inline constexpr std::uint16_t k_first_allocated = 1024;
// postgres holds a book id in a smallint and a client order id in a bigint, so the top bit stays clear
inline constexpr std::uint16_t k_last_allocated = 0x7FFF;

struct Book {
    std::uint16_t id{0};
    BookKind kind{BookKind::Unknown};
    std::string name{};
    UnixNanos created{};

    friend bool operator==(const Book&, const Book&) = default;
};

enum class BookError : std::uint8_t {
    BadName,
    NotFound,
    IdTaken,   // this number is on record under another name
    Exhausted, // every allocatable number has been handed out
    Store,     // the file would not read or write
};

[[nodiscard]] std::string_view to_string(BookError error) noexcept;

// what a person may log in as and a client may be called; it travels in a query string and a header
[[nodiscard]] bool valid_label(std::string_view label) noexcept;

// the kind a name means, Unknown when it is none of `manual`, `manual:<login>`, `client:<name>`, `hedge`, `desk`
[[nodiscard]] BookKind reserved_kind(std::string_view name) noexcept;

// the book a login places under; a missing or unusable login has the shared book
[[nodiscard]] std::string manual_book_name(std::string_view login);

// ids are never reused, so this is past the highest ever on record
[[nodiscard]] std::expected<std::uint16_t, BookError> next_id(std::span<const Book> on_record) noexcept;

} // namespace raikiri::books
