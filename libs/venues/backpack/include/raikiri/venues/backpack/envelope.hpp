#pragma once

#include <raikiri/wire/error.hpp>

#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::backpack {

using WireError = wire::Error;

enum class EventKind : std::uint8_t {
    BookTicker,
    Trade,
    Depth,
    MarkPrice, // mark, index and, on a perpetual only, funding
    Error,
    Unknown,
};

// The raw text of `data`; a frame with no wrapper is returned unchanged.
[[nodiscard]] std::expected<std::string_view, WireError> unwrap(std::string_view frame);

// No subscribe is acknowledged, even one naming an unlisted symbol: only the
// frames that arrive say what is subscribed.
[[nodiscard]] EventKind parse_event_kind(std::string_view payload);

struct RestError {
    std::string_view code; // a word, not a number
    std::string_view message;
};

// The body's error, or an empty code when the body is not one.
[[nodiscard]] std::expected<RestError, WireError> parse_rest_error(std::string_view body);

} // namespace raikiri::venues::backpack
