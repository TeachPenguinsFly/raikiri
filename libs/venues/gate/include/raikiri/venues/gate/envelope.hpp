#pragma once

// numeric fields arrive quoted or bare; t is milliseconds, create_time and funding_next_apply are seconds

#include <raikiri/venues/gate/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <simdjson.h>

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

namespace raikiri::venues::gate {

enum class EventKind : std::uint8_t {
    BookTicker,   // {futures,spot}.book_ticker
    Trade,        // {futures,spot}.trades
    Ticker,       // futures.tickers: funding rate and mark price
    BookUpdate,   // futures.order_book_update: deltas, needs a seed
    BookSnapshot, // spot.order_book: a complete book
    OrderUpdate,  // futures.orders / spot.orders, private
    UserTrade,    // futures.usertrades / spot.usertrades, private
    SubscribeAck,
    Error,
    Pong,
    Unknown,
};

// contract is filled only on control frames; data parsers read their own
struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view contract;
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// a refused subscription keeps event subscribe and adds an error with a numeric code
struct StreamError {
    std::int64_t code{0};
    std::string_view message;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// message is absent on some refusals, so only label is required
struct RestError {
    std::string_view label;
    std::string_view message;
};

// fails when the body has no label; callers check only non-2xx bodies
[[nodiscard]] std::expected<RestError, WireError> parse_rest_error(std::string_view body);

// GET /spot/time in milliseconds; futures has no equivalent
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// gate quotes prices, and sizes only under X-Gate-Size-Decimal, so either encoding is accepted
[[nodiscard]] std::expected<std::string_view, WireError> num_of(wire::ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_num(wire::ondemand::object& obj, std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_num(wire::ondemand::object& obj, std::string_view key);

// whole part of a decimal token, or 0; parse_decimal_e9 would overflow on epoch ms
[[nodiscard]] std::uint64_t whole_u64(std::string_view s) noexcept;

// absent yields 0; gate quotes some integer fields, which wire::opt_u64 refuses
[[nodiscard]] std::expected<std::uint64_t, WireError> num_u64(wire::ondemand::object& obj, std::string_view key);

// find_field_unordered only rewinds from the start, so both reset before looking
[[nodiscard]] inline std::string_view field(wire::ondemand::object& obj, std::string_view key) {
    obj.reset();
    return opt_num(obj, key).value_or(std::string_view{});
}

[[nodiscard]] inline std::string_view text_field(wire::ondemand::object& obj, std::string_view key) {
    obj.reset();
    return wire::opt_str(obj, key).value_or(std::string_view{});
}

// gate signs the size instead of naming a side; the view points into the caller's buffer
struct SignedSize {
    std::string_view magnitude;
    bool negative{false};
};

[[nodiscard]] inline SignedSize split_sign(std::string_view token) noexcept {
    const bool negative = token.starts_with('-');
    // remove_prefix since substr can throw
    if (negative || token.starts_with('+')) {
        token.remove_prefix(1);
    }
    return {.magnitude = token, .negative = negative};
}

// payloads hang off result: an object for book_ticker and order_book_update, an array for trades and tickers
template <typename Fn>
[[nodiscard]] auto with_result_object(wire::ondemand::object& root,
                                      Fn&& fn) -> decltype(fn(std::declval<wire::ondemand::object&>())) {
    auto obj = wire::req_object(root, "result");
    if (!obj.has_value()) {
        return std::unexpected(obj.error());
    }
    return std::forward<Fn>(fn)(*obj);
}

// walks an array of objects into caller storage
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
