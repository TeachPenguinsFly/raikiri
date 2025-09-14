#pragma once

// What every BitMEX answer is wrapped in, on both transports.
//
// A data frame is {"table":..,"action":"partial|insert|update|delete","data":[]}
// and `update` carries ONLY the columns that changed. Every field of every row
// below except the one identifying that row is therefore OPTIONAL — an empty
// view means ABSENT, never zero — and the mapper decides what is publishable.
// Requiring markPrice would reject most of the `instrument` stream; requiring
// both sides would reject a fifth of `quote` (both verified live).
//
// REST has no envelope of its own: a success is the payload and nothing else,
// and only a failure is wrapped, in {"error":{message,name}}.

#include <raikiri/venues/bitmex/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <simdjson.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace raikiri::venues::bitmex {

// Resolved from `table` once so dispatch is an integer compare.
enum class TableKind : std::uint8_t {
    Quote,      // top of book, one row per pool
    Trade,      // prints
    Instrument, // mark price, funding, index — and the whole catalogue row
    OrderBook,  // orderBookL2_25

    // Private tables. They ride a socket of their own — the same endpoint,
    // authenticated first — and naming them here rather than letting a default
    // swallow them is what makes a new private table a compiler error instead
    // of a silently dropped order update.
    Order,
    Execution,
    Position,

    SubscribeAck,
    Error,
    Info, // the connection banner
    Unknown,
};

// `partial` is BitMEX's word for a snapshot: it opens every subscription,
// re-opens it after a reconnect, and is the only action carrying every column.
enum class FrameAction : std::uint8_t { Partial, Insert, Update, Delete, None };

struct FrameInfo {
    TableKind table{TableKind::Unknown};
    FrameAction action{FrameAction::None};
};

// The keepalive answer is the bare text "pong", not JSON, so it lands here as
// NotJson and the caller ignores it — the same path as any frame BitMEX adds.
[[nodiscard]] std::expected<FrameInfo, WireError> parse_frame(std::string_view frame);

// {"status":400,"error":"Unknown or expired symbol: X",...}. Surfaced rather
// than dropped: a subscription that was never established and one that is
// merely quiet look identical downstream.
struct StreamError {
    std::int64_t status{0};
    std::string_view message;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// One ack per arg, not one per subscribe message.
struct SubscribeAck {
    std::string_view topic;
    bool success{false};
};

[[nodiscard]] std::expected<SubscribeAck, WireError> parse_subscribe_ack(std::string_view frame);

// The only error body BitMEX has. `name` is coarse — "HTTPError" covers both
// 403 and 404 — so classification leans on the HTTP status first.
struct RestError {
    std::string_view message;
    std::string_view name;
};

[[nodiscard]] std::expected<RestError, WireError> parse_rest_error(std::string_view body);

// Reads a JSON NUMBER as the digits BitMEX wrote, so parse_decimal_e9 sees
// "0.000000001" rather than whatever a double rounds it to. An absent field or
// a JSON null yields empty, which every caller reads as "not in this frame".
[[nodiscard]] std::expected<std::string_view, WireError> opt_num(wire::ondemand::object& obj,
                                                                 std::string_view key);

// Every websocket table wraps its rows in `data`, and the symbol-scoped
// subscriptions this adapter opens put exactly one row in the frames that are
// read as a single value.
template <typename Fn>
[[nodiscard]] auto with_first_data(wire::ondemand::object& root, Fn&& fn)
    -> decltype(fn(std::declval<wire::ondemand::object&>())) {
    auto array = wire::req_array(root, "data");
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    for (auto entry : *array) {
        wire::ondemand::object obj;
        if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        return std::forward<Fn>(fn)(obj);
    }
    // An empty data array carries no datum; reporting one would invent it.
    return std::unexpected(WireError::MissingField);
}

// REST answers a bare JSON array of rows; the websocket wraps the SAME rows in
// {"table":..,"action":..,"data":[..]}. Reading the first non-space byte is
// what lets one reader serve both, and a row read out of the wrong wrapper is
// the failure this avoids — an empty result that looks like an empty account.
//
// A template because the row reader differs per domain and nothing else does:
// the unwrapping, the capacity refusal and the per-row walk are identical for
// orders, executions and positions alike.
template <typename Row, typename Read>
[[nodiscard]] std::expected<std::span<const Row>, WireError>
read_rows(std::string_view payload, std::span<Row> into, Read&& read) {
    return wire::guarded([&]() -> std::expected<std::span<const Row>, WireError> {
        std::size_t at = 0;
        while (at < payload.size() && (payload[at] == ' ' || payload[at] == '\n' ||
                                       payload[at] == '\r' || payload[at] == '\t')) {
            ++at;
        }
        if (at == payload.size()) {
            return std::unexpected(WireError::NotJson);
        }
        std::optional<wire::ondemand::array> array;
        if (payload[at] == '[') {
            auto bare = wire::session().open_array(payload);
            if (!bare.has_value()) {
                return std::unexpected(bare.error());
            }
            array = *bare;
        } else {
            auto root = wire::session().open_object(payload);
            if (!root.has_value()) {
                return std::unexpected(root.error());
            }
            auto wrapped = wire::req_array(*root, "data");
            if (!wrapped.has_value()) {
                return std::unexpected(wrapped.error());
            }
            array = *wrapped;
        }

        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == into.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto row = read(obj);
            if (!row.has_value()) {
                return std::unexpected(row.error());
            }
            into[count] = *row;
            ++count;
        }
        return std::span<const Row>{into.data(), count};
    });
}

// Every string column but the row key is optional: an `update` action carries
// only what changed, and BitMEX keys `order` on orderID alone.
[[nodiscard]] inline std::expected<void, WireError>
read_strings(wire::ondemand::object& obj,
             std::initializer_list<std::pair<const char*, std::string_view*>> fields) {
    for (const auto& [key, out] : fields) {
        const auto value = wire::opt_str(obj, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *out = *value;
    }
    return {};
}

[[nodiscard]] inline std::expected<void, WireError>
read_numbers(wire::ondemand::object& obj,
             std::initializer_list<std::pair<const char*, std::string_view*>> fields) {
    for (const auto& [key, out] : fields) {
        const auto value = opt_num(obj, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *out = *value;
    }
    return {};
}

} // namespace raikiri::venues::bitmex
