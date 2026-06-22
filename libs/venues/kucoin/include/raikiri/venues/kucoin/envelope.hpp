#pragma once

// What every KuCoin answer is wrapped in, on both transports, and the readers
// every domain here needs before it can read anything of its own.
//
// A websocket frame names its `type`, its `subject` and the topic the symbol
// hides in; a REST body carries a string `code` and puts the payload under
// `data`. Neither belongs to any one domain, and both have to be read first.

#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

namespace raikiri::venues::kucoin {

// Resolved from type/subject once, so downstream dispatch is an integer
// compare rather than a string compare per message.
enum class EventKind : std::uint8_t {
    Bbo,         // subject tickerV2
    Trade,       // subject match, on /contractMarket/execution
    Depth,       // subject level2
    MarkPrice,   // subject mark.index.price, on /contract/instrument
    FundingRate, // subject funding.rate, on the same topic
    OrderChange, // subject orderChange (or symbolOrderChange), private topic
    Welcome,     // sent once on connect, before anything may be subscribed
    Ack,
    Pong,
    Error,
    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view symbol; // empty on control frames
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// The private topics carry no symbol behind their topic string — the payload
// names it instead — so the public discriminator, which reads the symbol out
// of the topic, refuses these frames outright.
[[nodiscard]] std::expected<EventInfo, WireError> parse_private_event(std::string_view frame);

// A refused subscription or a rejected token. `data` holds the text here,
// where a REST body would use `msg`.
struct StreamError {
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// "200000" is success. The code is a string, so it is converted once here and
// everything below works in integers.
inline constexpr std::int64_t k_ok = 200'000;

struct RestEnvelope {
    std::int64_t code{0};
    std::string_view msg;
};

// Fails when `code` is missing or is not a run of digits: a body whose envelope
// cannot be read must never be mistaken for one that said "200000".
[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

// POST /api/v1/bullet-public: a short-lived token plus the endpoint to use it
// against. Part of connecting rather than of startup, because it expires.
struct WireBullet {
    std::string_view token;
    std::string_view endpoint; // "wss://ws-api-futures.kucoin.com/"
    std::uint64_t ping_interval_ms{0};
    std::uint64_t ping_timeout_ms{0};
};

[[nodiscard]] std::expected<WireBullet, WireError> parse_bullet(std::string_view body);

// GET /api/v1/timestamp. `data` is a bare number here, not an object.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// "/contractMarket/tickerV2:XBTUSDTM" -> "XBTUSDTM". Every public channel
// hides the symbol in the topic and states it nowhere else, so the readers of
// five different payloads all start here.
[[nodiscard]] std::expected<std::string_view, WireError> topic_symbol(wire::ondemand::object& root);

// The scalar readers, which are KuCoin's own.
//
// THESE SHADOW THE SAME NAMES IN raikiri::wire AND DO NOT MEAN THE SAME THING.
// KuCoin writes half its numbers as JSON numbers and half as quoted strings —
// the same field differing between the spot and futures trees — so these work
// from the raw token with the quotes stripped, where the shared readers demand
// one JSON type and fail on the other. Inside this venue, reach for these; a
// `wire::opt_u64` on a quoted timestamp reads zero and says nothing.

// The token exactly as it was written, whitespace trimmed and escapes intact.
[[nodiscard]] std::expected<std::string_view, WireError> raw_of(wire::ondemand::value value);

// Numeric text with surrounding quotes removed, so that tickerV2's quoted prices
// and its bare sizes reach the DTO through one reader.
[[nodiscard]] std::expected<std::string_view, WireError> decimal_of(wire::ondemand::value value);
[[nodiscard]] std::int64_t i64_of_digits(std::string_view text) noexcept;

[[nodiscard]] std::expected<std::string_view, WireError> opt_decimal(wire::ondemand::object& obj,
                                                                     std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_decimal(wire::ondemand::object& obj,
                                                                     std::string_view key);

[[nodiscard]] std::expected<std::uint64_t, WireError> opt_u64(wire::ondemand::object& obj,
                                                              std::string_view key);
[[nodiscard]] std::expected<std::int64_t, WireError> opt_i64(wire::ondemand::object& obj,
                                                             std::string_view key);

// Both transports wrap the payload in `data`, and unlike okx it is a single
// object on every channel and on every singleton REST read.
template <typename Fn>
[[nodiscard]] auto with_data(wire::ondemand::object& root, Fn&& fn)
    -> decltype(fn(std::declval<wire::ondemand::object&>())) {
    auto obj = wire::req_object(root, "data");
    if (!obj.has_value()) {
        return std::unexpected(obj.error());
    }
    return std::forward<Fn>(fn)(*obj);
}

// The rows of a list, read into caller storage.
//
// A template because the row reader differs per domain and nothing else does:
// the capacity refusal and the per-row object walk are the same for orders,
// fills, positions and the spot catalogue alike. The array itself is the
// caller's, because KuCoin sometimes hands back `data` and sometimes `items`
// inside a paged `data`.
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

} // namespace raikiri::venues::kucoin
