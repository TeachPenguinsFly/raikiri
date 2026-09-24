#pragma once

#include <raikiri/venues/kraken/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::kraken {

enum class EventKind : std::uint8_t {
    Ticker, // bid/ask/mark/index/funding in one frame
    TickerLite, // ticker without the derivative fields
    Trade,      // trade and trade_snapshot
    Book,       // book and book_snapshot
    Subscribed, // subscribed / unsubscribed acknowledgement
    Alert,      // a refusal; see StreamAlert
    Info,       // {"event":"info","version":1} on connect

    Challenge,          // {"event":"challenge","message":"<uuid>"}
    OpenOrdersSnapshot, // the whole working set
    OpenOrders, // one delta, a different shape
    Fills,              // fills and fills_snapshot; one shape, both spellings

    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view product_id; // empty on control frames
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// a refused subscription: an alert with no code, so the text is the only handle
struct StreamAlert {
    std::string_view message;
};

[[nodiscard]] std::expected<StreamAlert, WireError> parse_stream_alert(std::string_view frame);

struct WireChallenge {
    std::string_view message; // the UUID to sign
};

[[nodiscard]] std::expected<WireChallenge, WireError> parse_challenge(std::string_view frame);

// serverTime has a variable-width fraction, so it is parsed, not sliced
struct RestEnvelope {
    bool ok{false};
    std::string_view error;     // e.g. "invalidArgument: symbol"; empty on success
    std::uint64_t server_ms{0}; // 0 when serverTime was absent or unreadable
};

// an unrouted request yields status NOT_FOUND and an errors array with no error string
[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

// epoch ms from an iso-8601 utc instant
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_iso8601_ms(std::string_view text);

// raw token text; kraken writes both 64110.0 and 8.907125e-6, so map.cpp parses it. null reads as empty
[[nodiscard]] std::expected<std::string_view, WireError> num_of(wire::ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_num(wire::ondemand::object& obj,
                                                                 std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_num(wire::ondemand::object& obj,
                                                                 std::string_view key);

// present tells an absent field from a real zero, since precision 0 is valid
[[nodiscard]] std::expected<std::int64_t, WireError>
opt_i64(wire::ondemand::object& obj, std::string_view key, bool& present);

[[nodiscard]] std::expected<std::uint64_t, WireError> opt_ms(wire::ondemand::object& obj,
                                                             std::string_view key);

} // namespace raikiri::venues::kraken
