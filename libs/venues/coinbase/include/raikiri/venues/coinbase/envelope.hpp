#pragma once

// What every Coinbase Exchange answer is wrapped in, on both transports.
//
// A websocket frame names itself in `type` and is otherwise flat — there is no
// payload key to unwrap. A REST body is the payload itself, and only a failure
// is wrapped, in {"message":"..."}.
//
// Every timestamp is an ISO-8601 STRING, never an epoch number, and the
// fractional part is 3, 6 or 9 digits depending on the endpoint (verified:
// /time sends 3, the ticker channel 6, /products/*/book 9). Nothing is
// converted here; map.cpp owns the parser and explains why it is hand-rolled.

#include <raikiri/venues/coinbase/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::coinbase {

// Resolved from the frame's "type" once, so downstream dispatch is an integer
// compare rather than a string compare per message.
enum class EventKind : std::uint8_t {
    Ticker,        // ticker — best bid/ask, republished on every match
    Match,         // match, and the last_match replay sent on subscribe
    Snapshot,      // level2_batch's opening book, whole
    L2Update,      // level2_batch's changes
    Subscriptions, // the subscribe confirmation
    Error,
    Unknown, // heartbeat and status — parsed by nobody here
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view product_id; // empty on control frames
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// {"type":"error","message":"Failed to subscribe","reason":"X is not a valid
// product"}. `reason` carries the detail; `message` alone says nothing about
// which subscription died.
struct StreamError {
    std::string_view message;
    std::string_view reason;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

inline constexpr std::size_t k_max_ack_channels = 16;

struct AckBuffer {
    std::array<std::string_view, k_max_ack_channels> channels{};
};

// The confirmation lists every channel now live on the connection, named by
// the venue rather than by us: a level2_batch request comes back as
// "level2_50" (verified live), so comparing the ack against what was asked
// for would read a confirmed channel as a refused one. An EMPTY array is the
// refusal signal — it is what arrives beside an error frame.
[[nodiscard]] std::expected<std::span<const std::string_view>, WireError>
parse_subscriptions(std::string_view frame, AckBuffer& storage);

// Returns /time's `iso` field. Its sibling `epoch` is a JSON floating-point
// number, which is exactly the representation this codebase refuses to make
// decisions on, so the string is the only usable form.
[[nodiscard]] std::expected<std::string_view, WireError> parse_server_time(std::string_view body);

// The whole of a Coinbase REST failure body: {"message":"NotFound"}.
[[nodiscard]] std::expected<std::string_view, WireError> parse_rest_error(std::string_view body);

} // namespace raikiri::venues::coinbase
