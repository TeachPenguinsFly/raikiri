#pragma once

// What every Bitfinex answer is wrapped in, and how a POSITIONAL array is read.
//
// Nothing here is named on the wire: a payload is an array of slots, and the
// reader below is what every domain module in this venue is built on.
//
//   1. Arity is declared once per schema as `k_*_arity` beside the layout
//      comment in the module that owns it, and `read_row<Arity>` refuses a
//      shorter array outright (WireError::ShortArray). LENGTH IS THE ONLY
//      STRUCTURAL CHECK THE FORMAT OFFERS — a venue that INSERTS a field
//      mid-row keeps the count plausible and silently renames every later
//      index, and no parser can see that. What length does catch is
//      truncation and appends.
//   2. Every index is a named `k_*` constant, never a literal at the read
//      site, and `Row<Arity>::at<I>()` static_asserts I < Arity, so a read
//      past a declared schema does not compile.
//   3. map.cpp asserts semantic invariants a one-slot shift would break
//      (bid < ask, sizes positive) — those, not the parser, are what would
//      catch an insertion.

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

// Which document root a frame has, decided from its first non-space byte.
// Cheaper and simpler than opening the document and asking its type, and the
// two shapes are the whole of Bitfinex's dispatch: `{` is a control frame,
// `[` is data.
[[nodiscard]] bool is_array_frame(std::string_view frame) noexcept;

// Raw JSON token of a value, as a view over the CALLER's buffer. Strings lose
// their quotes; `null` reads as empty.
//
// Numbers keep their exact spelling, scientific notation included — map.cpp's
// exponent-aware parser needs the token, and get_string() would move the bytes
// into simdjson's unescape buffer, which cannot be rebased. That lifetime bug
// has already cost one debug cycle in this repo.
[[nodiscard]] std::expected<std::string_view, WireError> token_of(wire::ondemand::value value);

// A positional row, read into a fixed number of raw tokens.
//
// Arity is a TEMPLATE parameter, not a runtime count, so at<I>() past the
// schema a caller declared is a compile error rather than a plausible number.
template <std::size_t Arity> struct Row {
    std::array<std::string_view, Arity> fields{};

    template <std::size_t Index> [[nodiscard]] std::string_view at() const noexcept {
        static_assert(Index < Arity, "positional read past the schema's declared arity");
        return fields[Index];
    }
};

// Reads the first `Arity` elements of a JSON array.
//
// A shorter array is ShortArray, never a partly-filled Row: on a positional
// wire, truncation means the reader cannot tell which field it stopped in the
// middle of. Extra trailing elements are ignored on purpose — Bitfinex already
// sends eleven ticker fields where it documents ten, and the `conf` TIMESTAMP
// flag appends to the tail of every envelope.
template <std::size_t Arity>
[[nodiscard]] std::expected<Row<Arity>, WireError> read_row(wire::ondemand::array array) {
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

template <std::size_t Arity>
[[nodiscard]] std::expected<Row<Arity>, WireError> read_row(wire::ondemand::value value) {
    wire::ondemand::array array;
    if (const auto ec = value.get_array().get(array); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    return read_row<Arity>(array);
}

// Signed variant for codes and level counts. Returns false rather than a
// plausible zero when the token is not an integer.
[[nodiscard]] bool token_to_i64(std::string_view s, std::int64_t& out) noexcept;

// The raw token of a NAMED field, for the handful of control frames Bitfinex
// sends as objects. Named apart from wire::opt_str because it is not the same
// reader: this one keeps a number's exact spelling and reads `null` as empty,
// the way every positional slot here does.
[[nodiscard]] std::expected<std::string_view, WireError> opt_token(wire::ondemand::object& obj,
                                                                   std::string_view key);

// What a socket frame is, decided from its first two elements (data frames)
// or its `event` field (control frames). A data frame names no instrument at
// all — only its channel id — which is why the venue layer keeps a
// chanId -> instrument registry and this enum stops at "Data".
enum class FrameKind : std::uint8_t {
    Info,         // {"event":"info",...}: greeting, or a coded venue notice
    Subscribed,   // carries the chanId every later frame is keyed by
    Unsubscribed, //
    Error,        // {"event":"error","code":10300,...}
    Pong,         //
    Heartbeat,    // [CHAN_ID,"hb"] — NOT a payload; publishing it is garbage
    Data,         // [CHAN_ID,<payload>] or [CHAN_ID,"te",<payload>]
    Auth,         // {"event":"auth","status":"OK"|"FAILED",...}
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

// Coded `info` notices. 20051 asks for a reconnect and 20061 ends a
// maintenance window; both REISSUE channel ids, which is why the venue layer
// treats them like a socket open rather than like a log line.
inline constexpr std::int64_t k_info_reconnect = 20051;
inline constexpr std::int64_t k_info_maintenance_start = 20060;
inline constexpr std::int64_t k_info_maintenance_end = 20061;

// The subscribe acknowledgement: the ONLY place a channel id is tied to an
// instrument. `symbol` names the pair on ticker/trades/book; the `status`
// channel names it inside `key` ("deriv:tBTCF0:USTF0") and carries no
// `symbol` at all, so both are surfaced.
struct WireSubscribed {
    std::int64_t chan_id{0};
    std::string_view channel; // ticker | trades | book | status
    std::string_view symbol;  // "tBTCUSD"; empty on the status channel
    std::string_view key;     // "deriv:tBTCF0:USTF0"; empty elsewhere
};

[[nodiscard]] std::expected<WireSubscribed, WireError> parse_subscribed(std::string_view frame);

// {"event":"error","code":10300,"msg":"symbol: invalid"} — code is a JSON
// NUMBER here, unlike the digit strings okx and bybit use.
struct StreamError {
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// The auth acknowledgement, which is an OBJECT unlike everything else here.
// `status` is "OK" or "FAILED"; on a failure `code` says why and `msg` is the
// only human-readable part.
struct WireAuthAck {
    bool ok{false};
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<WireAuthAck, WireError> parse_auth_ack(std::string_view frame);

// `["error",10020,"symbol: invalid"]`. An array, like everything else here,
// so an error body and a data body are told apart by their first element
// rather than by an envelope field.
inline constexpr std::size_t k_rest_error_arity = 3;
inline constexpr std::size_t k_rest_error_tag = 0;
inline constexpr std::size_t k_rest_error_code = 1;
inline constexpr std::size_t k_rest_error_msg = 2;

struct RestError {
    std::int64_t code{0};
    std::string_view msg;
};

// Cheap enough to run on every body before parsing it as data: an error is
// the only Bitfinex REST answer whose first element is a string.
[[nodiscard]] bool is_rest_error(std::string_view body) noexcept;

[[nodiscard]] std::expected<RestError, WireError> parse_rest_error(std::string_view body);

// `GET /v2/platform/status` answers `[1]`; zero is maintenance.
inline constexpr std::int64_t k_platform_operative = 1;

[[nodiscard]] std::expected<std::int64_t, WireError> parse_platform_status(std::string_view body);

// Walks a data envelope to its payload and hands it to `fn` along with the
// string tag that preceded it, if any.
//
// The envelope is variable-length BY DESIGN and is the one array here that
// must not be read with a fixed arity: `[CHAN_ID,<payload>]`,
// `[CHAN_ID,"te",<payload>]`, and either of those with a trailing send
// timestamp once the `conf` TIMESTAMP flag is on. Position 1 is therefore
// typed, not counted — a string there is a tag and an array is the payload.
template <typename Fn>
[[nodiscard]] auto with_payload(std::string_view frame, Fn&& fn)
    -> decltype(fn(std::declval<wire::ondemand::value>(), std::string_view{})) {
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

// The same walk for the private frames, whose tag the caller has already
// dispatched on: the socket wraps a payload as `[0,"<tag>",<payload>]` while
// REST answers the payload alone. Position 1 is TYPED, not counted, by the
// same rule as above.
template <typename Fn>
[[nodiscard]] auto with_row(std::string_view frame, Fn&& fn)
    -> decltype(fn(std::declval<wire::ondemand::value>())) {
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

// Walks a list of positional rows, which arrives bare from REST and wrapped in
// a socket envelope on a snapshot.
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

// `[[` is a bare list of rows and `[0,` a socket envelope carrying one. Decided
// on the first two bytes for the same reason is_array_frame is: it is the whole
// of the dispatch and the alternative is a rewind ondemand does not offer.
[[nodiscard]] inline bool is_row_list(std::string_view frame) noexcept {
    const auto text = wire::trim(frame);
    if (text.size() < 2 || text.front() != '[') {
        return false;
    }
    const auto rest = wire::trim(text.substr(1));
    return !rest.empty() && rest.front() == '[';
}

template <typename Row_, typename Read>
[[nodiscard]] std::expected<std::size_t, WireError>
rows_of(std::string_view frame, std::span<Row_> into, Read read) {
    if (is_row_list(frame)) {
        auto root = wire::session().open_array(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return read_rows<Row_>(*root, into, read);
    }
    return with_row(
        frame, [&](wire::ondemand::value payload) -> std::expected<std::size_t, WireError> {
            wire::ondemand::array rows;
            if (const auto ec = payload.get_array().get(rows); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            return read_rows<Row_>(rows, into, read);
        });
}

} // namespace raikiri::venues::bitfinex
