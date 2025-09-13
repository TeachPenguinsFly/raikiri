#pragma once

// What every BingX answer is wrapped in, on both trees and all three sockets.
//
// The swap and spot trees are separate deployments that happen to share this
// envelope. Where a frame differs, it is called out on the type: the shapes
// look close enough to read one as the other, and every place they diverge is
// somewhere a wrong read is silent rather than loud.
//
// The keepalive is the literal text "Ping" after inflation and must be
// answered with "Pong". It is not JSON, so parse_event resolves it before
// opening a document.
//
// The token readers below are here rather than in any one domain because two
// or more of them need each: BingX QUOTES every number on the perpetual tree
// and sends the same fields bare on spot, so the raw token is the only
// spelling that survives both.

#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::bingx {

// Resolved from `dataType` once, so dispatch is an integer compare per message.
enum class EventKind : std::uint8_t {
    BookTicker, // {sym}@bookTicker — top of book, one object
    Trade,      // {sym}@trade
    Depth,      // {sym}@depth100@100ms — a 100-level snapshot, not a delta
    MarkPrice,  // {sym}@markPrice — mark only; BingX streams no funding
    Ticker,     // {sym}@ticker — the rolling 24-hour window, both trees
    Ping,       // the bare text "Ping"
    ControlAck, // a subscribe/unsubscribe answer with code 0
    Error,      // the same shape with a non-zero code
    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view symbol; // empty on control frames and on Ping
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// The keepalive, before any JSON parser sees it. parse_event resolves it for
// the public socket; the user streams need it too and have no dataType to
// dispatch on — and a socket that stops answering "Pong" is closed.
[[nodiscard]] bool is_ping(std::string_view frame) noexcept;

// The answer to a sub/unsub, echoing the id that asked. A refusal is the same
// shape with a non-zero code, so the two are one field apart — and a refusal
// read as an acknowledgement looks exactly like a stream that is merely quiet.
struct ControlFrame {
    std::string_view id;
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<ControlFrame, WireError> parse_control(std::string_view frame);

enum class PrivateKind : std::uint8_t {
    OrderUpdate,      // ORDER_TRADE_UPDATE (perp) / spot.executionReport
    ListenKeyExpired, // the key died; the socket cannot recover without a new one
    Ack,              // a subscribe answer with code 0 (spot only; perp needs none)
    Error,            // the same shape with a non-zero code
    Ignorable,        // ACCOUNT_UPDATE / ACCOUNT_CONFIG_UPDATE: no sink channel
    Unknown,
};

[[nodiscard]] std::expected<PrivateKind, WireError> parse_private_event(std::string_view frame);

inline constexpr std::int64_t k_ok = 0;

struct RestEnvelope {
    std::int64_t code{0};
    std::string_view msg;
};

// Every REST body is {"code":<number>,"msg":...,"data":...}, but a failure
// drops `data` for `timestamp`, so only `code` and `msg` can be relied on.
[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// Shared token readers ----------------------------------------------------

// For the numbers where zero is a meaningful value rather than an absence: a
// missing `code` must not read as success, and a missing pricePrecision must
// not read as a tick size of 1.
[[nodiscard]] std::expected<std::int64_t, WireError> req_i64(wire::ondemand::object& obj,
                                                             std::string_view key);

// "BTC-USDT@depth100@100ms" -> "BTC-USDT". The channel suffix may itself contain
// '@', so the split is on the FIRST one. Empty when there is no '@' at all.
// Not noexcept only because substr is not; every caller is inside guarded().
[[nodiscard]] std::string_view symbol_of(std::string_view data_type);

// Reads any scalar as a view over the caller's buffer: a number keeps its
// token, a string loses its quotes, JSON null reads as empty. One reader for
// both because BingX QUOTES every number on the perpetual tree and sends the
// same fields bare on spot — `q:"10.00000000"` beside `q:5` for the same
// quantity, in two pushes of the same order's life.
[[nodiscard]] std::expected<std::string_view, WireError> scalar_of(wire::ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_scalar(wire::ondemand::object& obj,
                                                                    std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_scalar(wire::ondemand::object& obj,
                                                                    std::string_view key);

// A millisecond stamp however it was spelled. Absent reads as zero, which
// map.cpp turns into local ingest time rather than into 1970.
[[nodiscard]] std::expected<std::uint64_t, WireError> opt_ms_any(wire::ondemand::object& obj,
                                                                 std::string_view key);

} // namespace raikiri::venues::bingx
