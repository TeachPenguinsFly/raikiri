#pragma once

// a refused subscribe fails the whole frame, so frames hold only channels the catalogue vouched for

#include <raikiri/venues/weex/errors.hpp>
#include <raikiri/wire/error.hpp>
#include <raikiri/wire/json.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::weex {

enum class EventKind : std::uint8_t {
    Depth,         // <sym>@depth200: a delta, d == "CHANGED"
    DepthSnapshot, // the same channel's opening frame, d == "SNAPSHOT"
    Trade,         // <sym>@trade: prints as they happen
    TradeSnapshot, // the same channel's opening backfill; see market_data.hpp
    Ticker,        // <sym>@ticker: the 24-hour window, mark and index
    BookTicker,    // <sym>@bookTicker: spot's top of book
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

// peeks at "e", then the control keys
[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// weex states no numeric code on the socket, only text after "INVALID_ARGUMENT: "
struct StreamError {
    std::uint64_t id{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// no rest envelope: a negative code marks an error; an unrouted path has no code and fails the status check
struct RestError {
    std::int64_t code{0};
    std::string_view msg;
};

// zero code means the body is a success payload
[[nodiscard]] std::expected<RestError, WireError> parse_rest_error(std::string_view body);

// {"serverTime":<ms>}; zero when absent
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// a bare array body into caller storage
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

// the same rows under a named key, as the ledger's items
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
