#pragma once

// the keepalive is the text "Ping", answered with "Pong"; it is not json
// perp quotes every number and spot sends it bare, so readers take raw tokens

#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::bingx {

enum class EventKind : std::uint8_t {
    BookTicker, // {sym}@bookTicker
    Trade,      // {sym}@trade
    Depth, // {sym}@depth100@100ms, a full snapshot
    MarkPrice, // {sym}@markPrice; no funding
    Ticker, // {sym}@ticker, rolling 24h window
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

// checked before json parsing; an unanswered ping closes the socket
[[nodiscard]] bool is_ping(std::string_view frame) noexcept;

// a refusal has the same shape with a non-zero code
struct ControlFrame {
    std::string_view id;
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<ControlFrame, WireError> parse_control(std::string_view frame);

enum class PrivateKind : std::uint8_t {
    OrderUpdate,      // ORDER_TRADE_UPDATE (perp) / spot.executionReport
    ListenKeyExpired, // the key expired; reconnect with a new one
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

// a failure replaces data with timestamp; only code and msg are reliable
[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// for values where zero is meaningful, so absence must fail
[[nodiscard]] std::expected<std::int64_t, WireError> req_i64(wire::ondemand::object& obj,
                                                             std::string_view key);

// split on the first '@'; empty when there is none
[[nodiscard]] std::string_view symbol_of(std::string_view data_type);

// any scalar as a view; perp quotes numbers and spot sends them bare
[[nodiscard]] std::expected<std::string_view, WireError> scalar_of(wire::ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_scalar(wire::ondemand::object& obj,
                                                                    std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_scalar(wire::ondemand::object& obj,
                                                                    std::string_view key);

// absent reads as zero; map.cpp substitutes ingest time
[[nodiscard]] std::expected<std::uint64_t, WireError> opt_ms_any(wire::ondemand::object& obj,
                                                                 std::string_view key);

} // namespace raikiri::venues::bingx
