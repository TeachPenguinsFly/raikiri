#pragma once

// What every XT answer is wrapped in, on all four transports, and the readers
// every domain here needs before it can read anything of its own.
//
// TWO EXCHANGES UNDER ONE BRAND, and the wrappers differ as much as the
// payloads: the futures REST body carries `returnCode` beside an `error`
// object, the spot one carries `rc` and `mc`, the public socket names a
// `topic` and hides the payload under a `data` OBJECT, and the private socket
// answers a subscribe with a bare code. None of that belongs to a domain, and
// all of it has to be read first.

#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

namespace raikiri::venues::xt {

// Resolved from the frame's `topic` once, so downstream dispatch is an integer
// compare rather than a string compare per message.
enum class EventKind : std::uint8_t {
    Depth,       // depth — top-N snapshot, the BBO and book source
    DepthUpdate, // depth_update — incremental deltas
    Trade,       // trade
    MarkPrice,   // mark_price
    Ticker,      // ticker — the rolling 24-hour window
    Ack,         // {"code":0,...} — a subscription XT accepted
    Error,       // {"code":1,...} — a subscription XT refused
    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view symbol; // empty on control frames
};

// XT answers a bad `method` with the bare text "Invalid parameter", not JSON,
// so that fails here and the caller ignores it — the same path as any frame XT
// adds later.
[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// A control frame. `code` is 0 on an accepted subscription and 1 on every
// refusal XT has been seen to produce; `msg` is the only thing that
// distinguishes one failure from another, so it is surfaced verbatim.
struct StreamAck {
    std::int64_t code{0};
    std::string_view msg;
    std::string_view id; // echoed from the subscribe frame; absent on some errors
};

[[nodiscard]] std::expected<StreamAck, WireError> parse_stream_ack(std::string_view frame);

inline constexpr std::int64_t k_ok = 0;

// `code` is empty when the call succeeded. Both halves matter: returnCode says
// whether to look, error.code says what happened.
struct RestEnvelope {
    std::int64_t return_code{0};
    std::string_view code;
    std::string_view msg;
};

// Fails rather than reporting success when returnCode is missing or is not a
// number: a body whose envelope cannot be read must never be mistaken for a
// body that said 0.
[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

// Spot answers {"rc":0,"mc":"SUCCESS","ma":[],"result":...}: `rc` carries one
// bit and `mc` carries the identity of the failure, which is what decides
// retry from halt. Fails rather than reporting success when `rc` is missing.
inline constexpr std::int64_t k_spot_ok = 0;

struct SpotEnvelope {
    std::int64_t rc{0};
    std::string_view mc;
};

[[nodiscard]] std::expected<SpotEnvelope, WireError> parse_spot_envelope(std::string_view body);

// /public/time answers with the epoch milliseconds as a bare number in
// `result`, not an object.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// /v4/public/time answers `result.serverTime` as a JSON number — an object,
// unlike the futures clock, whose `result` is the number itself.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_spot_server_time(std::string_view body);

// The spot user-stream token, `result.accessToken`, and the futures one,
// whose `result` is the listen key STRING itself. Facts about the connection
// rather than about any one market, which is why they are read here.
[[nodiscard]] std::expected<std::string_view, WireError> parse_spot_ws_token(std::string_view body);

[[nodiscard]] std::expected<std::string_view, WireError> parse_listen_key(std::string_view body);

// A private-stream frame: {"topic":"order","event":"order@<key>","data":{...}}.
// The topic is what decides the reader; the event repeats it with the listen
// key glued on and carries nothing else.
enum class PrivateTopic : std::uint8_t {
    Order,
    Trade,
    Position,
    Balance,
    Ack,     // {"id":..,"code":0,"msg":"success"} — a subscribe XT accepted
    Refusal, // the same shape with a non-zero code; msg names the failure
    Unknown,
};

struct PrivateFrame {
    PrivateTopic topic{PrivateTopic::Unknown};
    std::int64_t code{0}; // Ack and Refusal only
    std::string_view msg; // Ack and Refusal only
};

[[nodiscard]] std::expected<PrivateFrame, WireError> parse_private_frame(std::string_view frame);

// The scalar readers, which are XT's own.
//
// XT puts its timestamps in JSON numbers and its sequence ids in digit
// strings, so both a numeric and a textual reader are needed where okx needed
// only the textual one. `opt_ms` SHADOWS the raikiri::wire name and does not
// mean the same thing, and `opt_i64` takes a caller-chosen absent value where
// the shared one takes none — inside this venue, reach for these.

[[nodiscard]] std::expected<std::int64_t, WireError> req_i64(wire::ondemand::object& obj,
                                                             std::string_view key);
[[nodiscard]] std::expected<bool, WireError> req_bool(wire::ondemand::object& obj,
                                                      std::string_view key);

// XT's `t` is epoch milliseconds as a JSON number. Reading it as a number
// keeps it away from parse_decimal_e9, which scales by 1e9 and overflows int64
// on an epoch-ms value.
[[nodiscard]] std::expected<std::uint64_t, WireError> opt_ms(wire::ondemand::object& obj,
                                                             std::string_view key);

// Sequence ids (`u`, `pu`, `fu`, `id`) are digit STRINGS. Anything that is not
// exactly a run of digits yields 0.
[[nodiscard]] std::expected<std::uint64_t, WireError> req_digits(wire::ondemand::object& obj,
                                                                 std::string_view key);

// Reads any scalar as a view over the caller's buffer: a number keeps its
// token, a string loses its quotes, JSON null reads as empty. The private
// tree needs both encodings for the SAME field — the socket quotes `price`
// while /order/list ships it as a bare number — so one reader covers them.
[[nodiscard]] std::expected<std::string_view, WireError> scalar_of(wire::ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_scalar(wire::ondemand::object& obj,
                                                                    std::string_view key);

// Signed, with a caller-chosen absent value: the private DTOs use negative
// sentinels where a field may legitimately be missing.
[[nodiscard]] std::expected<std::int64_t, WireError>
opt_i64(wire::ondemand::object& obj, std::string_view key, std::int64_t fallback);

[[nodiscard]] std::expected<bool, WireError>
opt_bool_or(wire::ondemand::object& obj, std::string_view key, bool fallback);

// Copies a decimal-or-id field with opt_scalar, because XT ships the SAME
// field in both encodings depending on the endpoint: /order/detail quotes
// nothing and /order/list-open-order quotes everything.
[[nodiscard]] inline std::expected<void, WireError>
take(wire::ondemand::object& obj, std::string_view key, std::string_view& into) {
    const auto value = opt_scalar(obj, key);
    if (!value.has_value()) {
        return std::unexpected(value.error());
    }
    into = *value;
    return {};
}

// Every public channel wraps its payload in a `data` OBJECT — there is no
// array to index and no empty-array case to guard, unlike okx.
template <typename Fn>
[[nodiscard]] auto with_data(wire::ondemand::object& root, Fn&& fn)
    -> decltype(fn(std::declval<wire::ondemand::object&>())) {
    auto data = wire::req_object(root, "data");
    if (!data.has_value()) {
        return std::unexpected(data.error());
    }
    return std::forward<Fn>(fn)(*data);
}

// `result` is an ARRAY on the unpaged reads (/v4/open-order, position/list) and
// an OBJECT wrapping `items` on the paged ones (/v4/trade, order/trade-list).
// Asking for one shape and getting the other is not an error XT reports — the
// read simply returns nothing — so the shape is resolved before the walk.
[[nodiscard]] std::expected<wire::ondemand::array, WireError> rows_of(wire::ondemand::object& root,
                                                                      std::string_view key);

// The rows themselves, read into caller storage.
//
// A template because the row reader differs per domain and nothing else does:
// the capacity refusal and the per-row object walk are the same for orders,
// deals, positions and balances alike.
template <typename Row, typename Read>
[[nodiscard]] std::expected<std::size_t, WireError>
read_rows(wire::ondemand::array array, std::span<Row> into, Read read) {
    std::size_t count = 0;
    for (auto entry : array) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        wire::ondemand::object obj;
        if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        Row row{};
        if (const auto ok = read(obj, row); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        into[count] = row;
        ++count;
    }
    return count;
}

} // namespace raikiri::venues::xt
