#pragma once

// What every backpack answer is wrapped in, on both transports.
//
// A websocket frame is {"data":{...},"stream":"<type>.<symbol>"} and names its
// event in data.e. A REST failure is an HTTP status beside
// {"code":"INVALID_CLIENT_REQUEST","message":"..."} — the code is a WORD, not
// a number, so nothing here can reuse a neighbour's negative-integer reader.

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
    Error,     // {"error":{...},"id":N}
    Unknown,
};

// Unwraps the stream envelope to the raw text of `data`. A frame with no
// wrapper is returned unchanged.
[[nodiscard]] std::expected<std::string_view, WireError> unwrap(std::string_view frame);

// Reads data.e off an already unwrapped payload.
//
// NO ACKNOWLEDGEMENT EXISTS TO READ. Measured 2026-09-24: a SUBSCRIBE is
// answered by nothing at all, and one naming a symbol the venue does not list
// — bookTicker.NOPE_USDC — is answered by nothing too, and never delivers.
// Only the frames that arrive say what is subscribed.
[[nodiscard]] EventKind parse_event_kind(std::string_view payload);

struct RestError {
    std::string_view code;
    std::string_view message;
};

// The body's error, or an empty code when the body is not one.
[[nodiscard]] std::expected<RestError, WireError> parse_rest_error(std::string_view body);

} // namespace raikiri::venues::backpack
