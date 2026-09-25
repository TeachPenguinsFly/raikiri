#pragma once

#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

namespace raikiri::venues::xt {

enum class EventKind : std::uint8_t {
    Depth,       // top-N snapshot, the bbo and book source
    DepthUpdate, // incremental deltas
    Trade,
    MarkPrice,
    Ticker, // rolling 24-hour window
    Ack,    // {"code":0,...}, accepted subscription
    Error,  // {"code":1,...}, refused subscription
    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view symbol; // empty on control frames
};

// a bad method answers plain text "Invalid parameter", which fails here and is ignored
[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// code 0 accepts, 1 refuses; msg is the only detail
struct StreamAck {
    std::int64_t code{0};
    std::string_view msg;
    std::string_view id; // echoed from the subscribe frame; absent on some errors
};

[[nodiscard]] std::expected<StreamAck, WireError> parse_stream_ack(std::string_view frame);

inline constexpr std::int64_t k_ok = 0;

// code is empty on success; returnCode says whether to look, error.code what happened
struct RestEnvelope {
    std::int64_t return_code{0};
    std::string_view code;
    std::string_view msg;
};

// fails when returnCode is missing or not a number
[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

// spot answers {"rc":0,"mc":"SUCCESS",...}; mc identifies the failure
inline constexpr std::int64_t k_spot_ok = 0;

struct SpotEnvelope {
    std::int64_t rc{0};
    std::string_view mc;
};

[[nodiscard]] std::expected<SpotEnvelope, WireError> parse_spot_envelope(std::string_view body);

// result is the epoch ms as a bare number
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// result.serverTime, unlike the futures clock
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_spot_server_time(std::string_view body);

// spot reads result.accessToken; futures' result is the listen key string
[[nodiscard]] std::expected<std::string_view, WireError> parse_spot_ws_token(std::string_view body);

[[nodiscard]] std::expected<std::string_view, WireError> parse_listen_key(std::string_view body);

// {"topic":"order","event":"order@<key>","data":{...}}; topic picks the reader
enum class PrivateTopic : std::uint8_t {
    Order,
    Trade,
    Position,
    Balance,
    Ack,     // {"id":..,"code":0,"msg":"success"}
    Refusal, // the same shape with a non-zero code; msg names the failure
    Unknown,
};

struct PrivateFrame {
    PrivateTopic topic{PrivateTopic::Unknown};
    std::int64_t code{0}; // Ack and Refusal only
    std::string_view msg; // Ack and Refusal only
};

[[nodiscard]] std::expected<PrivateFrame, WireError> parse_private_frame(std::string_view frame);

// opt_ms and opt_i64 shadow raikiri::wire names with different meanings; use these inside this venue

[[nodiscard]] std::expected<std::int64_t, WireError> req_i64(wire::ondemand::object& obj, std::string_view key);
[[nodiscard]] std::expected<bool, WireError> req_bool(wire::ondemand::object& obj, std::string_view key);

// epoch ms as a json number; parse_decimal_e9 would overflow on it
[[nodiscard]] std::expected<std::uint64_t, WireError> opt_ms(wire::ondemand::object& obj, std::string_view key);

// digit-string sequence ids; anything else yields 0
[[nodiscard]] std::expected<std::uint64_t, WireError> req_digits(wire::ondemand::object& obj, std::string_view key);

// a number keeps its token, a string loses its quotes, null reads empty
[[nodiscard]] std::expected<std::string_view, WireError> scalar_of(wire::ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_scalar(wire::ondemand::object& obj, std::string_view key);

// the private dtos use negative sentinels for absent fields
[[nodiscard]] std::expected<std::int64_t, WireError>
opt_i64(wire::ondemand::object& obj, std::string_view key, std::int64_t fallback);

[[nodiscard]] std::expected<bool, WireError>
opt_bool_or(wire::ondemand::object& obj, std::string_view key, bool fallback);

// xt quotes the same field on one endpoint and not on another
[[nodiscard]] inline std::expected<void, WireError>
take(wire::ondemand::object& obj, std::string_view key, std::string_view& into) {
    const auto value = opt_scalar(obj, key);
    if (!value.has_value()) {
        return std::unexpected(value.error());
    }
    into = *value;
    return {};
}

// public payloads sit in a data object
template <typename Fn>
[[nodiscard]] auto with_data(wire::ondemand::object& root,
                             Fn&& fn) -> decltype(fn(std::declval<wire::ondemand::object&>())) {
    auto data = wire::req_object(root, "data");
    if (!data.has_value()) {
        return std::unexpected(data.error());
    }
    return std::forward<Fn>(fn)(*data);
}

// result is an array on unpaged reads and an object with items on paged ones; the wrong shape reads empty
[[nodiscard]] std::expected<wire::ondemand::array, WireError> rows_of(wire::ondemand::object& root,
                                                                      std::string_view key);

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
