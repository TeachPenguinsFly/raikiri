#pragma once

// What every WEEX answer is wrapped in, on both transports, and the
// connection-level reads that belong to no market at all.
//
// A data frame names its event in "e", its symbol in "s" and its send time in
// "E", with the payload under "d" for everything except depth, whose levels sit
// at the top level under "b" and "a". Control frames carry no "e" at all: the
// keepalive and the greeting name themselves in "event", and a subscribe verdict
// is a bare {"result":bool,"id":n}. A REST body has no envelope and states
// failure as a NEGATIVE `code` beside the payload's own fields.
//
// THE VERDICT IS ONE BOOLEAN. WEEX answers an accepted subscribe with
// {"result":true,"id":1} and a refused one with {"result":false,"id":1,"msg":
// "INVALID_ARGUMENT: ..."} — one field apart, and reading a refusal as an
// acknowledgement is indistinguishable from a channel the venue simply never
// sends. Worse, the refusal is for the WHOLE FRAME: a single bad channel in a
// params array of eight left all eight unsubscribed and pushed nothing, which
// is why a frame is built only from channels the catalogue vouched for.

#include <raikiri/venues/weex/errors.hpp>
#include <raikiri/wire/error.hpp>
#include <raikiri/wire/json.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::weex {

// Resolved from the frame's event type once, so downstream dispatch is an
// integer compare rather than a string compare per message.
enum class EventKind : std::uint8_t {
    Depth,         // <sym>@depth15: a delta, d == "CHANGED"
    DepthSnapshot, // the same channel's opening frame, d == "SNAPSHOT"
    Trade,         // <sym>@trade: prints as they happen
    TradeSnapshot, // the same channel's opening backfill; see market_data.hpp
    Ticker,        // <sym>@ticker: the 24-hour window, mark and index
    Ping,          // {"event":"ping","time":"..."}
    Connected,     // {"cid":"...","event":"connected","time":"..."}
    SubscribeAck,  // {"result":true,"id":1}
    Error,         // {"result":false,"id":1,"msg":"..."}
    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view symbol; // empty for control frames
};

// Peeks at "e" and, failing that, at the control keys, without consuming the
// rest of the frame.
[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// A refused SUBSCRIBE. WEEX states no numeric code on the socket — the reason
// is prose behind an "INVALID_ARGUMENT: " prefix — so this carries the text and
// the id it answered, and nothing pretends to a code that was never sent.
struct StreamError {
    std::uint64_t id{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// WEEX has no REST envelope: a success is the payload itself and a failure is
// {"code":-1142,"msg":"Parameter 'symbol' is invalid."} at a 4xx. So an error is
// detected by the presence of a NEGATIVE `code`, not by a status field.
//
// A path WEEX does not route answers something else again — {"timestamp":...,
// "status":404,"error":"Not Found","path":"..."} with no `code` at all — which
// reads here as a success payload and is caught by the status check the caller
// does anyway.
struct RestError {
    std::int64_t code{0};
    std::string_view msg;
};

// Returns the error when the body is one; absence is expressed as a zero code,
// meaning "this body is a success payload".
[[nodiscard]] std::expected<RestError, WireError> parse_rest_error(std::string_view body);

// {"serverTime":<ms>}. Read once by the signed half, because WEEX refuses a
// signature stamped more than 30 seconds from ITS clock and a laptop that has
// drifted wears the face of a revoked key. Zero when the body does not say.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// A BARE ARRAY body, read into caller storage.
//
// The private reads answer with the array itself and no wrapper at all —
// positions, balances, open orders and fills are each a top-level `[...]` —
// so the shape that wants a key is the exception here rather than the rule.
// A template because only the row reader differs: the capacity refusal and the
// per-row object walk are the same for every one of them.
template <typename Entry, typename Read>
[[nodiscard]] std::expected<std::span<const Entry>, WireError>
read_array(std::string_view body, std::span<Entry> storage, Read&& read) {
    auto root = wire::session().open_array(body);
    if (!root.has_value()) {
        return std::unexpected{root.error()};
    }
    std::size_t count = 0;
    for (auto item : *root) {
        if (count == storage.size()) {
            return std::unexpected{WireError::TooManyEntries};
        }
        wire::ondemand::object entry_obj;
        if (item.get_object().get(entry_obj) != simdjson::SUCCESS) {
            return std::unexpected{WireError::WrongType};
        }
        Entry entry{};
        if (auto ok = read(entry_obj, entry); !ok.has_value()) {
            return std::unexpected{ok.error()};
        }
        storage[count++] = entry;
    }
    return std::span<const Entry>{storage.data(), count};
}

// The same rows under a named key. One private read is shaped this way —
// the account ledger, whose rows sit under `items` beside its cursor.
template <typename Entry, typename Read>
[[nodiscard]] std::expected<std::span<const Entry>, WireError>
read_list(std::string_view body, std::string_view key, std::span<Entry> storage, Read&& read) {
    auto root = wire::session().open_object(body);
    if (!root.has_value()) {
        return std::unexpected{root.error()};
    }
    auto obj = *root;
    auto rows = wire::req_array(obj, key);
    if (!rows.has_value()) {
        return std::unexpected{rows.error()};
    }
    std::size_t count = 0;
    for (auto item : *rows) {
        if (count == storage.size()) {
            return std::unexpected{WireError::TooManyEntries};
        }
        wire::ondemand::object entry_obj;
        if (item.get_object().get(entry_obj) != simdjson::SUCCESS) {
            return std::unexpected{WireError::WrongType};
        }
        Entry entry{};
        if (auto ok = read(entry_obj, entry); !ok.has_value()) {
            return std::unexpected{ok.error()};
        }
        storage[count++] = entry;
    }
    return std::span<const Entry>{storage.data(), count};
}

} // namespace raikiri::venues::weex
