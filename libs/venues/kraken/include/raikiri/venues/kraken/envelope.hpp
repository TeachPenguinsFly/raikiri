#pragma once

// What every Kraken Futures answer is wrapped in.
//
// One socket carries the public feeds and the private ones; what separates
// them is the signed challenge on the subscribe, so the challenge frame and
// the feed classification live together. REST has no envelope of its own
// beyond `result`, and no clock endpoint at all — `serverTime` rides on every
// response, which is why the ISO-8601 reader is here rather than in a domain.
//
// The token readers below are here rather than in any one domain because two
// or more of them need each: Kraken writes `64110.0` and `8.907125e-6` for the
// same field on one socket, so nothing here interprets a number — map.cpp owns
// the parser that reads both spellings.

#include <raikiri/venues/kraken/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::kraken {

// Resolved from `feed` (or `event`) once, so dispatch is an integer compare.
enum class EventKind : std::uint8_t {
    Ticker,     // ticker — bid/ask/mark/index/funding in one frame
    TickerLite, // ticker_lite — the same frame with the derivative fields cut
    Trade,      // trade and trade_snapshot
    Book,       // book and book_snapshot
    Subscribed, // subscribed / unsubscribed acknowledgement
    Alert,      // a refusal; see StreamAlert
    Info,       // {"event":"info","version":1} on connect

    // Private feeds. The socket is the same one the public feeds ride; what
    // separates them is the signed challenge on the subscribe.
    Challenge,          // {"event":"challenge","message":"<uuid>"}
    OpenOrdersSnapshot, // open_orders_snapshot — the whole working set
    OpenOrders,         // open_orders — one delta, a different SHAPE
    Fills,              // fills and fills_snapshot; one shape, both spellings

    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view product_id; // empty on control frames
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// A refused subscription. VERIFIED: a bad product id answers
// {"event":"alert","message":"Couldn't subscribe to invalid product `X`"} —
// `alert`, not `error`, with no code field, so the text is the only handle.
struct StreamAlert {
    std::string_view message;
};

[[nodiscard]] std::expected<StreamAlert, WireError> parse_stream_alert(std::string_view frame);

struct WireChallenge {
    std::string_view message; // the UUID to sign
};

[[nodiscard]] std::expected<WireChallenge, WireError> parse_challenge(std::string_view frame);

// Kraken has no clock endpoint: `serverTime` rides on every response.
// VERIFIED: its fractional part is variable width ("...:08.64Z" and
// "...:16.918Z" in one session), so it is parsed, never sliced.
struct RestEnvelope {
    bool ok{false};
    std::string_view error;     // e.g. "invalidArgument: symbol"; empty on success
    std::uint64_t server_ms{0}; // 0 when serverTime was absent or unreadable
};

// Two failure shapes exist. The documented one names `error`; an unrouted
// request instead yields {"status":"NOT_FOUND",...,"errors":[...]} with no
// `error` string at all.
[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

// Epoch milliseconds from an ISO-8601 UTC instant. Exposed because Kraken
// mixes both encodings and a caller must never guess which it holds.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_iso8601_ms(std::string_view text);

// Shared token readers ----------------------------------------------------

// Reads a JSON NUMBER as raw token text. Kraken emits `64110.0` and
// `8.907125e-6` for the same field on one socket, so nothing here interprets
// it: map.cpp owns the parser that reads both. JSON null reads as empty.
[[nodiscard]] std::expected<std::string_view, WireError> num_of(wire::ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_num(wire::ondemand::object& obj,
                                                                 std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_num(wire::ondemand::object& obj,
                                                                 std::string_view key);

// `present` distinguishes an absent field from a real zero. That matters for
// contractValueTradePrecision, where 0 is a valid precision and defaulting to
// it would silently give an unknown instrument a step of exactly 1.
[[nodiscard]] std::expected<std::int64_t, WireError>
opt_i64(wire::ondemand::object& obj, std::string_view key, bool& present);

[[nodiscard]] std::expected<std::uint64_t, WireError> opt_ms(wire::ondemand::object& obj,
                                                             std::string_view key);

} // namespace raikiri::venues::kraken
