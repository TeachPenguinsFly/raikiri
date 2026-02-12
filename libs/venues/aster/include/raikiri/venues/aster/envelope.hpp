#pragma once

// What every aster answer is wrapped in, on both transports, and the
// connection-level reads that belong to no market at all.
//
// A websocket frame names its event in "e" and may arrive inside a
// {"stream":...,"data":{...}} wrapper; a REST body has no envelope and states
// failure as a NEGATIVE `code` beside the payload's own fields. Neither
// belongs to any one domain, and both have to be read before anything else
// can be.

#include <raikiri/venues/aster/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::aster {

// Resolved from the frame's event type once, so downstream dispatch is an
// integer compare rather than a string compare per message.
enum class EventKind : std::uint8_t {
    BookTicker,    // <sym>@bookTicker
    Trade,         // <sym>@trade, or <sym>@aggTrade
    MarkPrice,     // <sym>@markPrice — mark, index and funding in one frame
    DepthUpdate,   // <sym>@depth
    OrderUpdate,   // user stream: ORDER_TRADE_UPDATE
    AccountUpdate, // user stream: ACCOUNT_UPDATE
    ListenKeyExpired,
    SubscribeAck, // {"result":null,"id":1}
    Error,        // {"error":{"code":2,"msg":"..."},"id":1}
    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view symbol; // empty for control and account frames
};

// Peeks at the wrapper and the inner "e" without consuming the rest.
[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// The same, on a payload that has ALREADY been unwrapped.
//
// FOUR PARSES OF ONE FRAME is what the convenient form costs on the market
// data path, and every one of them is a memcpy of the frame plus a full
// simdjson structural scan: parse_event unwraps (1) and opens the payload (2),
// then the caller unwraps again (3) and opens the payload again (4) to read
// the fields. Profiled on aster 2026-09-18, the parse path was 8.6% of the
// gateway. Unwrap once, pass the payload down, and one of the four is gone
// with no change in what is read.
[[nodiscard]] std::expected<EventInfo, WireError> parse_event_payload(std::string_view payload);

// Unwraps {"stream":...,"data":{...}} to the raw text of `data`. A frame that
// arrived on a single-stream socket has no wrapper and is returned unchanged.
[[nodiscard]] std::expected<std::string_view, WireError> unwrap(std::string_view frame);

// A refused SUBSCRIBE. Aster answers success with {"result":null,"id":N} and
// failure with an "error" object under the same id, so the two are one field
// apart — and a rejected subscription that reads as an acknowledgement is
// indistinguishable from a stream the venue simply never sends.
struct StreamError {
    std::int64_t code{0};
    std::string_view msg;
    std::uint64_t id{0};
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// Aster has no envelope: a success is the payload itself and a failure is
// {"code":-1121,"msg":"Invalid symbol."}. So an error is detected by the
// presence of a negative `code`, not by a status field — and `code` is a
// legitimate field name inside some success payloads, which is why this
// checks the sign rather than the key alone.
struct RestError {
    std::int64_t code{0};
    std::string_view msg;
};

// Returns the error when the body is one; std::nullopt-shaped absence is
// expressed as a zero code, meaning "this body is a success payload".
[[nodiscard]] std::expected<RestError, WireError> parse_rest_error(std::string_view body);

[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// {"listenKey":"..."} — the user-stream key, valid 60 minutes and extended by
// a keepalive rather than reissued. A fact about the connection, not about
// any one market, which is why it is read here and not in orders.
[[nodiscard]] std::expected<std::string_view, WireError> parse_listen_key(std::string_view body);

// Reads a run of string fields off one object in a single pass. Orders,
// positions and the account book each read most of a row this way, so the
// walk lives here rather than in whichever of them was written first.
struct Wanted {
    std::string_view key;
    std::string_view* into;
};

[[nodiscard]] inline std::expected<void, WireError> read_strings(wire::ondemand::object& obj,
                                                                 std::span<const Wanted> wanted) {
    for (const auto& want : wanted) {
        const auto value = wire::opt_str(obj, want.key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *want.into = *value;
    }
    return {};
}

} // namespace raikiri::venues::aster
