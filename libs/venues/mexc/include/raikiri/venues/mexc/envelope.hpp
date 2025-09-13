#pragma once

// What every MEXC answer is wrapped in, on all three transports.
//
// There is no `arg` object to peel on the futures socket. A data frame is
// {"channel":"push.ticker","symbol":"BTC_USDT","data":{...},"ts":...} and both
// the channel and the symbol sit at the ENVELOPE level, `data` being the
// payload alone. VERIFIED live for ticker, deal, depth and depth.full.
//
// A refused subscription is {"channel":"rs.error","data":"<text>"} — the
// reason is a human-readable string and there is NO code anywhere in the
// frame. An unknown `method` gets no answer at all (verified: sub.bogus was
// silently dropped while the two malformed sub.ticker calls each drew an
// rs.error), so silence is not evidence a channel was accepted.
//
// The futures REST envelope is {"success":...,"code":...,"data":...}; the spot
// REST bodies carry no envelope at all and report a refusal by growing a
// `code`. The spot socket is PROTOBUF, and its envelope is field numbers.

#include <raikiri/venues/mexc/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <simdjson.h>

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

namespace raikiri::venues::mexc {

// Resolved from the envelope `channel` once, so downstream dispatch is an
// integer compare rather than a string compare per message.
enum class EventKind : std::uint8_t {
    Ticker,       // push.ticker — bid1/ask1, fair price and funding rate
    Trade,        // push.deal
    Depth,        // push.depth — incremental
    DepthFull,    // push.depth.full — limit-N snapshot
    SubscribeAck, // rs.sub.*
    Pong,         // answer to {"method":"ping"}
    Error,        // rs.error

    // Private, and only after a successful login on the same socket. MEXC
    // pushes these unbidden — there is no sub.personal.* to send, so silence
    // after login is indistinguishable from an account with nothing to say.
    PersonalOrder,    // push.personal.order
    PersonalDeal,     // push.personal.order.deal
    PersonalPosition, // push.personal.position
    LoginAck,         // rs.login

    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view symbol; // empty on control frames
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// No code field exists to carry: classification has nothing to key on, so the
// caller records the text and counts the refusal.
struct StreamError {
    std::string_view msg;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

inline constexpr std::int64_t k_ok = 0;

struct RestEnvelope {
    bool success{false};
    std::int64_t code{0};
    std::string_view msg;
};

// Both halves are required: an error body carries success:false and a nonzero
// code, and a body whose envelope cannot be read must never be mistaken for
// one that said success.
[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

// /contract/ping, whose `data` is the epoch-millisecond number itself.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// Spot refusals are {"code":10072,"msg":"..."} and successes carry no `code`
// at all, so an absent code is success rather than an unreadable body.
struct SpotError {
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<SpotError, WireError> parse_spot_error(std::string_view body);

// /api/v3/time, whose body is {"serverTime":<epoch ms>}.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_spot_server_time(std::string_view body);

// Scalars ----------------------------------------------------------------

// Reads any scalar as a view over the caller's buffer: a number keeps its
// token, a string loses its quotes, JSON null reads as empty. One reader for
// both because MEXC mixes the two encodings for the same kind of value —
// push.deal's `t` is a number and its `cts` the same instant quoted.
[[nodiscard]] std::expected<std::string_view, WireError> scalar_of(wire::ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_scalar(wire::ondemand::object& obj,
                                                                    std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_scalar(wire::ondemand::object& obj,
                                                                    std::string_view key);

// Signed, and with a caller-chosen absent value, because the scale and state
// fields are absent-or-negative sentinels in the DTOs and because the depth
// version chain is compared as a number. NOT wire::opt_i64, which pins the
// absent case to 0 — a scale of 0 is a real precision.
[[nodiscard]] std::expected<std::int64_t, WireError>
opt_int(wire::ondemand::object& obj, std::string_view key, std::int64_t fallback);

// A millisecond stamp however MEXC encoded it.
//
// NOT wire::opt_ms, which reads through opt_str and so refuses a bare number:
// MEXC quotes `cts` and leaves `t` unquoted on the same channel.
[[nodiscard]] std::expected<std::uint64_t, WireError> scalar_ms(wire::ondemand::object& obj,
                                                                std::string_view key);

// Row readers reach fields out of order, and On-Demand only walks forward, so
// both of these rewind before looking.
[[nodiscard]] inline std::string_view field(wire::ondemand::object& obj, std::string_view key) {
    obj.reset();
    return opt_scalar(obj, key).value_or(std::string_view{});
}

[[nodiscard]] inline std::int64_t int_field(wire::ondemand::object& obj, std::string_view key) {
    obj.reset();
    return opt_int(obj, key, 0).value_or(0);
}

// The symbol sits beside `channel` in the envelope, not inside `data`. Reading
// it first keeps the object walk forward on a data frame, where MEXC emits it
// before the payload.
[[nodiscard]] inline std::expected<std::string_view, WireError>
envelope_symbol(wire::ondemand::object& root) {
    return opt_scalar(root, "symbol");
}

// A `data` array inside MEXC's futures envelope.
//
// Templated because the row reader differs per domain and nothing else does:
// the envelope, the capacity refusal and the per-row walk are the same for
// orders, deals and positions alike.
template <typename Entry, typename Read>
std::expected<std::span<const Entry>, WireError>
read_envelope_array(std::string_view body, std::span<Entry> into, Read&& read) {
    auto root = wire::session().open_object(body);
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    auto array = wire::req_array(*root, "data");
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto element : *array) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        wire::ondemand::object obj;
        if (const auto ec = element.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        Entry entry{};
        if (const auto ok = read(obj, entry); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        into[count] = entry;
        ++count;
    }
    return std::span<const Entry>{into.data(), count};
}

// A `data` object inside MEXC's futures envelope — the socket push and the
// single-order REST reads.
template <typename Entry, typename Read>
std::expected<Entry, WireError> read_envelope_object(std::string_view body, Read&& read) {
    auto root = wire::session().open_object(body);
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    auto data = wire::req_object(*root, "data");
    if (!data.has_value()) {
        return std::unexpected(data.error());
    }
    Entry entry{};
    if (const auto ok = read(*data, entry); !ok.has_value()) {
        return std::unexpected(ok.error());
    }
    return entry;
}

// A BARE top-level array — the spot bodies, which carry no envelope.
template <typename Entry, typename Read>
std::expected<std::span<const Entry>, WireError>
read_bare_array(std::string_view body, std::span<Entry> into, Read&& read) {
    auto array = wire::session().open_array(body);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto element : *array) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        wire::ondemand::object obj;
        if (const auto ec = element.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        Entry entry{};
        if (const auto ok = read(obj, entry); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        into[count] = entry;
        ++count;
    }
    return std::span<const Entry>{into.data(), count};
}

// The spot socket ---------------------------------------------------------
//
// MEXC spot v3 market data is PROTOBUF and not JSON. The JSON endpoint still
// accepts a connection and then refuses every subscription with "Not
// Subscribed successfully! ... Reason: Blocked!", so this is the only spot
// market data MEXC serves. Nothing else in this repository speaks protobuf;
// the reader is in libs/wire and decodes fields by number without a schema,
// because a .proto kept in step with a venue that ships unannounced changes is
// a liability rather than an asset.
//
// FIELD NUMBERS ARE STATED, NOT DERIVED. Every one used here and in the domain
// modules was read off a live capture; the fixtures in tests/venues/mexc are
// those captures. A number that moves makes a parser find nothing, which is a
// refusal — never a silently wrong price.

// The envelope every spot frame shares: #1 channel, #3 symbol, #6 send time.
// The payload rides in one of a handful of high field numbers, and which one
// is present is the only thing that says what the frame is.
enum class StreamKind : std::uint8_t {
    Quote, // #315 PublicAggreBookTickerV3Api
    Trade, // #314 PublicAggreDealsV3Api
    Depth, // #303 PublicLimitDepthsV3Api
    Unknown,
};

struct StreamEvent {
    StreamKind kind{StreamKind::Unknown};
    std::string_view symbol;
    std::uint64_t ts_ms{0};
    std::string_view payload; // the submessage, still encoded
};

[[nodiscard]] std::expected<StreamEvent, WireError> parse_stream_event(std::string_view frame);

} // namespace raikiri::venues::mexc
