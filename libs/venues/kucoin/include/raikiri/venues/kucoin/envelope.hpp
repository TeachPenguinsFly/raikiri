#pragma once

#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

namespace raikiri::venues::kucoin {

enum class EventKind : std::uint8_t {
    Bbo,         // subject tickerV2, spot level1
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

// private topics name the symbol in the payload only
[[nodiscard]] std::expected<EventInfo, WireError> parse_private_event(std::string_view frame);

// data holds the error text where rest would use msg
struct StreamError {
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// "200000" is success
inline constexpr std::int64_t k_ok = 200'000;

struct RestEnvelope {
    std::int64_t code{0};
    std::string_view msg;
};

// fails unless code is a run of digits
[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

// short-lived token plus the endpoint to use it against; fetched per connect
struct WireBullet {
    std::string_view token;
    std::string_view endpoint; // "wss://ws-api-futures.kucoin.com/"
    std::uint64_t ping_interval_ms{0};
    std::uint64_t ping_timeout_ms{0};
};

[[nodiscard]] std::expected<WireBullet, WireError> parse_bullet(std::string_view body);

// data is a bare number here
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// public channels state the symbol only in the topic
[[nodiscard]] std::expected<std::string_view, WireError> topic_symbol(wire::ondemand::object& root);

// kucoin's scalar readers accept quoted and bare numbers, unlike the raikiri::wire ones of the same name

// the raw token, trimmed, escapes intact
[[nodiscard]] std::expected<std::string_view, WireError> raw_of(wire::ondemand::value value);

// numeric text with quotes stripped
[[nodiscard]] std::expected<std::string_view, WireError> decimal_of(wire::ondemand::value value);
[[nodiscard]] std::int64_t i64_of_digits(std::string_view text) noexcept;

[[nodiscard]] std::expected<std::string_view, WireError> opt_decimal(wire::ondemand::object& obj, std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_decimal(wire::ondemand::object& obj, std::string_view key);

[[nodiscard]] std::expected<std::uint64_t, WireError> opt_u64(wire::ondemand::object& obj, std::string_view key);
[[nodiscard]] std::expected<std::int64_t, WireError> opt_i64(wire::ondemand::object& obj, std::string_view key);

// data is a single object on every channel and singleton rest read
template <typename Fn>
[[nodiscard]] auto with_data(wire::ondemand::object& root,
                             Fn&& fn) -> decltype(fn(std::declval<wire::ondemand::object&>())) {
    auto obj = wire::req_object(root, "data");
    if (!obj.has_value()) {
        return std::unexpected(obj.error());
    }
    return std::forward<Fn>(fn)(*obj);
}

// rows of a list into caller storage; the caller picks data or data.items
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
