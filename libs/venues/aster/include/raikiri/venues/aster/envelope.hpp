#pragma once

#include <raikiri/venues/aster/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::aster {

enum class EventKind : std::uint8_t {
    BookTicker,    // <sym>@bookTicker
    Trade,         // <sym>@trade, or <sym>@aggTrade
    MarkPrice,     // <sym>@markPrice: mark, index and funding
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

// peeks at the wrapper and the inner "e" without consuming the rest
[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// for a payload already unwrapped; saves a second unwrap and parse
[[nodiscard]] std::expected<EventInfo, WireError> parse_event_payload(std::string_view payload);

// a single-stream frame has no wrapper and is returned unchanged
[[nodiscard]] std::expected<std::string_view, WireError> unwrap(std::string_view frame);

// success is {"result":null,"id":N}; failure is an "error" object under the same id
struct StreamError {
    std::int64_t code{0};
    std::string_view msg;
    std::uint64_t id{0};
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// a failure is a body with a negative code; code also appears in some success payloads
struct RestError {
    std::int64_t code{0};
    std::string_view msg;
};

// a zero code means the body is a success payload
[[nodiscard]] std::expected<RestError, WireError> parse_rest_error(std::string_view body);

[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// listen key is valid 60 minutes and extended by keepalive
[[nodiscard]] std::expected<std::string_view, WireError> parse_listen_key(std::string_view body);

// reads several string fields off one object in a single pass
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
