#pragma once

// What every Gate.io v4 answer is wrapped in, on both transports.
//
// A websocket frame names its `channel` and `event` and hides the payload
// under `result`; a REST body is a bare array or object and reports a refusal
// by growing a `label`. Neither belongs to any one domain, and both have to be
// read first.
//
// The scalar readers live here for the same reason. Gate's numeric fields
// arrive quoted or bare with no rule that predicts which, and its epoch fields
// come in both units without the field name settling it: `t` is milliseconds;
// `create_time` and `funding_next_apply` are seconds. Worse, the REST
// /futures/{settle}/trades endpoint ships `create_time_ms` as FRACTIONAL
// SECONDS (1786394934.376) where the websocket ships the same field name as
// integer milliseconds — verified live, 2026-08. Only the websocket form is
// parsed here.

#include <raikiri/venues/gate/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <simdjson.h>

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

namespace raikiri::venues::gate {

// Resolved from the frame's channel and event once, so downstream dispatch is
// an integer compare rather than a string compare per message.
enum class EventKind : std::uint8_t {
    BookTicker,   // {futures,spot}.book_ticker — top of book
    Trade,        // {futures,spot}.trades
    Ticker,       // futures.tickers — carries funding rate AND mark price
    BookUpdate,   // futures.order_book_update — depth deltas, needs a seed
    BookSnapshot, // spot.order_book — a complete book, no chain to keep
    OrderUpdate,  // futures.orders / spot.orders — private
    UserTrade,    // futures.usertrades / spot.usertrades — private
    SubscribeAck,
    Error,
    Pong,
    Unknown,
};

// `contract` is filled only on control frames, where it comes from the echoed
// `payload`. Data frames carry it in four different places depending on the
// channel, so their own parsers report it rather than paying for the descent
// twice.
struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view contract;
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// A refused subscription: the frame keeps event:"subscribe" and adds
// {"error":{"code":2,"message":"unknown currency pair NOPE_USDT"}}. Unlike the
// REST side the code here is a NUMBER. Code 4 nests a REST-style label inside
// the message string; classification still works off the number.
struct StreamError {
    std::int64_t code{0};
    std::string_view message;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// `message` is absent on some refusals (CONTRACT_NOT_FOUND ships the label
// alone), so only the label is required.
struct RestError {
    std::string_view label;
    std::string_view message;
};

// Fails when the body carries no `label`, which is how a success body reports
// itself. Callers gate this on a non-2xx status rather than probing every
// body: the catalogue is over a megabyte and indexing it twice is waste.
[[nodiscard]] std::expected<RestError, WireError> parse_rest_error(std::string_view body);

// GET /spot/time — milliseconds as a JSON number; futures has no equivalent.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// Scalars ----------------------------------------------------------------

// Reads a numeric field whatever its JSON type. Gate quotes prices and leaves
// sizes bare, and its own SDK disagrees with the live endpoint about
// `order_size_min`, so requiring either encoding would break on real data.
[[nodiscard]] std::expected<std::string_view, WireError> num_of(wire::ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_num(wire::ondemand::object& obj,
                                                                 std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_num(wire::ondemand::object& obj,
                                                                 std::string_view key);

// Whole part of a decimal token. Gate types several epoch fields as float64 in
// its own SDK and ships the REST book's `current` as 1786394534.46, so the
// fraction is truncated rather than making the field unreadable. Not
// parse_decimal_e9: that scales by 1e9 and an epoch-ms value overflows int64.
// Anything else yields 0, which the mapper reads as "no venue time".
[[nodiscard]] std::uint64_t whole_u64(std::string_view s) noexcept;

// A numeric field as a whole number; absent yields 0.
//
// NOT wire::opt_u64, which demands a bare JSON number: Gate quotes `id` and
// `server_time` and leaves `t` and `u` bare, so the shared reader refuses half
// of them with WrongType.
[[nodiscard]] std::expected<std::uint64_t, WireError> num_u64(wire::ondemand::object& obj,
                                                              std::string_view key);

// Row readers reach fields out of order, and find_field_unordered only rewinds
// from the start of the object — so both of these reset before looking.
[[nodiscard]] inline std::string_view field(wire::ondemand::object& obj, std::string_view key) {
    obj.reset();
    return opt_num(obj, key).value_or(std::string_view{});
}

[[nodiscard]] inline std::string_view text_field(wire::ondemand::object& obj,
                                                 std::string_view key) {
    obj.reset();
    return wire::opt_str(obj, key).value_or(std::string_view{});
}

// Gate signs a size instead of naming a side, on public prints and private
// fills alike. The magnitude is the quantity and the sign is the direction, so
// both come out of one token; the stripped view still points into the caller's
// buffer.
struct SignedSize {
    std::string_view magnitude;
    bool negative{false};
};

[[nodiscard]] inline SignedSize split_sign(std::string_view token) noexcept {
    const bool negative = token.starts_with('-');
    // remove_prefix, not substr: substr can throw and this is noexcept.
    if (negative || token.starts_with('+')) {
        token.remove_prefix(1);
    }
    return {.magnitude = token, .negative = negative};
}

// Every websocket payload hangs off `result`. book_ticker and
// order_book_update put an object there; trades and tickers put an array whose
// elements are the objects.
template <typename Fn>
[[nodiscard]] auto with_result_object(wire::ondemand::object& root, Fn&& fn)
    -> decltype(fn(std::declval<wire::ondemand::object&>())) {
    auto obj = wire::req_object(root, "result");
    if (!obj.has_value()) {
        return std::unexpected(obj.error());
    }
    return std::forward<Fn>(fn)(*obj);
}

// Walks an array of objects into caller storage. The caller decides whether
// the array is the document root (REST) or hangs off `result` (websocket).
//
// Templated because the row reader differs per domain and nothing else does:
// the capacity refusal and the per-row walk are the same for orders, user
// trades, positions, ledger rows and currency pairs alike.
template <typename Entry, typename Read>
std::expected<std::span<const Entry>, WireError>
read_array(wire::ondemand::array items, std::span<Entry> storage, Read&& read) {
    std::size_t count = 0;
    for (auto item : items) {
        if (count == storage.size()) {
            return std::unexpected{WireError::TooManyEntries};
        }
        wire::ondemand::object obj;
        if (item.get_object().get(obj) != simdjson::SUCCESS) {
            return std::unexpected{WireError::WrongType};
        }
        Entry entry{};
        if (auto ok = read(obj, entry); !ok.has_value()) {
            return std::unexpected{ok.error()};
        }
        storage[count] = entry;
        ++count;
    }
    return std::span<const Entry>{storage.data(), count};
}

} // namespace raikiri::venues::gate
