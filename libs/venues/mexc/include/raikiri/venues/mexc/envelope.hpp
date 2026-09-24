#pragma once

// futures frames carry channel and symbol at the top level; rs.error
// is prose and an unknown method gets no reply

#include <raikiri/venues/mexc/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <simdjson.h>

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

namespace raikiri::venues::mexc {

enum class EventKind : std::uint8_t {
    Ticker, // push.ticker: bid1/ask1, fair price and funding rate
    Trade,        // push.deal
    Depth, // push.depth, incremental
    DepthFull, // push.depth.full, limit-n snapshot
    SubscribeAck, // rs.sub.*
    Pong,         // answer to {"method":"ping"}
    Error,        // rs.error

    // pushed unbidden after login; there is no sub.personal.*
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

[[nodiscard]] EventKind kind_from_channel(std::string_view channel) noexcept;

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// no code field, so the caller records the text and counts the refusal
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

// an error body has success:false and a nonzero code; an unreadable envelope is not success
[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

// /contract/ping, whose `data` is the epoch-millisecond number itself.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// spot refusals are {"code":10072,"msg":"..."}; success carries no `code`
struct SpotError {
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<SpotError, WireError> parse_spot_error(std::string_view body);

// /api/v3/time, whose body is {"serverTime":<epoch ms>}.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_spot_server_time(std::string_view body);

// numbers keep their token, strings lose quotes, null reads empty; mexc mixes the encodings
[[nodiscard]] std::expected<std::string_view, WireError> scalar_of(wire::ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_scalar(wire::ondemand::object& obj,
                                                                    std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_scalar(wire::ondemand::object& obj,
                                                                    std::string_view key);

// signed with a caller-chosen absent value; wire::opt_i64 pins absent to 0, which is a real scale
[[nodiscard]] std::expected<std::int64_t, WireError>
opt_int(wire::ondemand::object& obj, std::string_view key, std::int64_t fallback);

// accepts a quoted or bare number; mexc quotes `cts` and leaves `t` bare
[[nodiscard]] std::expected<std::uint64_t, WireError> scalar_ms(wire::ondemand::object& obj,
                                                                std::string_view key);

// on-demand only walks forward, so both rewind before looking
[[nodiscard]] inline std::string_view field(wire::ondemand::object& obj, std::string_view key) {
    obj.reset();
    return opt_scalar(obj, key).value_or(std::string_view{});
}

[[nodiscard]] inline std::int64_t int_field(wire::ondemand::object& obj, std::string_view key) {
    obj.reset();
    return opt_int(obj, key, 0).value_or(0);
}

// the symbol precedes `data` in the envelope, so read it first
[[nodiscard]] inline std::expected<std::string_view, WireError>
envelope_symbol(wire::ondemand::object& root) {
    return opt_scalar(root, "symbol");
}

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

// a bare top-level array, as in the spot bodies
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

// spot market data is protobuf only; field numbers come from live captures in tests/venues/mexc

// #1 channel, #3 symbol, #6 send time; the payload's field number gives the kind
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
