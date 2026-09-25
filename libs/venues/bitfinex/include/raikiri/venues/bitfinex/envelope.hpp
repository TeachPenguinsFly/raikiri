#pragma once

// read_row<Arity> refuses short rows and at<I> is bounds-checked at compile time
// an inserted field shifts later indices undetected; map.cpp's invariants catch it

#include <raikiri/venues/bitfinex/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <simdjson.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

namespace raikiri::venues::bitfinex {

// '{' is a control frame, '[' is data
[[nodiscard]] bool is_array_frame(std::string_view frame) noexcept;

// raw token as a view over the caller's buffer; strings unquoted, null empty
[[nodiscard]] std::expected<std::string_view, WireError> token_of(wire::ondemand::value value);

// arity is a template parameter so at<I>() past it does not compile
template <std::size_t Arity> struct Row {
    std::array<std::string_view, Arity> fields{};

    template <std::size_t Index> [[nodiscard]] std::string_view at() const noexcept {
        static_assert(Index < Arity, "positional read past the schema's declared arity");
        return fields[Index];
    }
};

// a shorter array is ShortArray; extra trailing elements are ignored
template <std::size_t Arity> [[nodiscard]] std::expected<Row<Arity>, WireError> read_row(wire::ondemand::array array) {
    Row<Arity> out{};
    std::size_t index = 0;
    for (auto entry : array) {
        if (index == Arity) {
            break;
        }
        wire::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto token = token_of(value);
        if (!token.has_value()) {
            return std::unexpected(token.error());
        }
        out.fields[index] = *token;
        ++index;
    }
    if (index < Arity) {
        return std::unexpected(WireError::ShortArray);
    }
    return out;
}

template <std::size_t Arity> [[nodiscard]] std::expected<Row<Arity>, WireError> read_row(wire::ondemand::value value) {
    wire::ondemand::array array;
    if (const auto ec = value.get_array().get(array); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    return read_row<Arity>(array);
}

// false when the token is not an integer
[[nodiscard]] bool token_to_i64(std::string_view s, std::int64_t& out) noexcept;

// named-field raw token for object control frames; keeps number spelling, null reads empty
[[nodiscard]] std::expected<std::string_view, WireError> opt_token(wire::ondemand::object& obj, std::string_view key);

// data frames carry only a channel id; the venue layer maps it to an instrument
enum class FrameKind : std::uint8_t {
    Info,       // {"event":"info",...}: greeting, or a coded venue notice
    Subscribed, // carries the chanId every later frame is keyed by
    Unsubscribed,
    Error, // {"event":"error","code":10300,...}
    Pong,
    Heartbeat, // [chan_id,"hb"], not a payload
    Data,      // [CHAN_ID,<payload>] or [CHAN_ID,"te",<payload>]
    Auth,      // {"event":"auth","status":"OK"|"FAILED",...}
    Unknown,
};

struct FrameInfo {
    FrameKind kind{FrameKind::Unknown};
    std::int64_t chan_id{0}; // Data and Heartbeat only
    std::int64_t code{0};    // Info and Error; zero on the plain greeting
    std::string_view tag;    // "te"/"tu" on trade updates, empty otherwise
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<FrameInfo, WireError> parse_frame(std::string_view frame);

// both reissue channel ids, so they are handled like a socket open
inline constexpr std::int64_t k_info_reconnect = 20051;
inline constexpr std::int64_t k_info_maintenance_start = 20060;
inline constexpr std::int64_t k_info_maintenance_end = 20061;

// the only place a channel id is tied to an instrument; status names it in key instead of symbol
struct WireSubscribed {
    std::int64_t chan_id{0};
    std::string_view channel; // ticker | trades | book | status
    std::string_view symbol;  // "tBTCUSD"; empty on the status channel
    std::string_view key;     // "deriv:tBTCF0:USTF0"; empty elsewhere
};

[[nodiscard]] std::expected<WireSubscribed, WireError> parse_subscribed(std::string_view frame);

// code is a json number here
struct StreamError {
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// an object; status is "OK" or "FAILED", with code and msg on failure
struct WireAuthAck {
    bool ok{false};
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<WireAuthAck, WireError> parse_auth_ack(std::string_view frame);

// ["error",code,text]; told apart from data by its first element
inline constexpr std::size_t k_rest_error_arity = 3;
inline constexpr std::size_t k_rest_error_tag = 0;
inline constexpr std::size_t k_rest_error_code = 1;
inline constexpr std::size_t k_rest_error_msg = 2;

struct RestError {
    std::int64_t code{0};
    std::string_view msg;
};

// an error is the only rest answer whose first element is a string
[[nodiscard]] bool is_rest_error(std::string_view body) noexcept;

[[nodiscard]] std::expected<RestError, WireError> parse_rest_error(std::string_view body);

// `GET /v2/platform/status` answers `[1]`; zero is maintenance.
inline constexpr std::int64_t k_platform_operative = 1;

[[nodiscard]] std::expected<std::int64_t, WireError> parse_platform_status(std::string_view body);

// variable length: position 1 is a tag if a string, the payload if an array
template <typename Fn>
[[nodiscard]] auto with_payload(std::string_view frame,
                                Fn&& fn) -> decltype(fn(std::declval<wire::ondemand::value>(), std::string_view{})) {
    auto root = wire::session().open_array(frame);
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    std::size_t index = 0;
    std::string_view tag;
    for (auto entry : *root) {
        wire::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        if (index == 0) {
            ++index; // channel id; the caller already dispatched on it
            continue;
        }
        wire::ondemand::json_type type{};
        if (const auto ec = value.type().get(type); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        if (index == 1 && type == wire::ondemand::json_type::string) {
            const auto token = token_of(value);
            if (!token.has_value()) {
                return std::unexpected(token.error());
            }
            tag = *token;
            ++index;
            continue;
        }
        if (type != wire::ondemand::json_type::array) {
            return std::unexpected(WireError::WrongType);
        }
        return std::forward<Fn>(fn)(value, tag);
    }
    return std::unexpected(WireError::MissingField);
}

// private frames are [0,"<tag>",<payload>]; rest answers the payload alone
template <typename Fn>
[[nodiscard]] auto with_row(std::string_view frame, Fn&& fn) -> decltype(fn(std::declval<wire::ondemand::value>())) {
    auto root = wire::session().open_array(frame);
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    std::size_t index = 0;
    for (auto entry : *root) {
        wire::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        wire::ondemand::json_type type{};
        if (const auto ec = value.type().get(type); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        if (type == wire::ondemand::json_type::array) {
            return std::forward<Fn>(fn)(value);
        }
        ++index;
        if (index > 2) {
            break; // a channel id and a tag are all that may precede a payload
        }
    }
    return std::unexpected(WireError::MissingField);
}

// bare from rest, wrapped in a socket envelope on a snapshot
template <typename Row_, typename Read>
[[nodiscard]] std::expected<std::size_t, WireError>
read_rows(wire::ondemand::array rows, std::span<Row_> into, Read read) {
    std::size_t count = 0;
    for (auto entry : rows) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        wire::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto row = read(value);
        if (!row.has_value()) {
            return std::unexpected(row.error());
        }
        into[count] = *row;
        ++count;
    }
    return count;
}

// '[[' is a bare row list, '[0,' a socket envelope
[[nodiscard]] inline bool is_row_list(std::string_view frame) noexcept {
    const auto text = wire::trim(frame);
    if (text.size() < 2 || text.front() != '[') {
        return false;
    }
    const auto rest = wire::trim(text.substr(1));
    return !rest.empty() && rest.front() == '[';
}

template <typename Row_, typename Read>
[[nodiscard]] std::expected<std::size_t, WireError> rows_of(std::string_view frame, std::span<Row_> into, Read read) {
    if (is_row_list(frame)) {
        auto root = wire::session().open_array(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return read_rows<Row_>(*root, into, read);
    }
    return with_row(frame, [&](wire::ondemand::value payload) -> std::expected<std::size_t, WireError> {
        wire::ondemand::array rows;
        if (const auto ec = payload.get_array().get(rows); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        return read_rows<Row_>(rows, into, read);
    });
}

} // namespace raikiri::venues::bitfinex
