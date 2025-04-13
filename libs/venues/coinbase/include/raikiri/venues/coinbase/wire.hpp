#pragma once

// Coinbase Exchange (formerly Coinbase Pro) wire types and parsers.
//
// Same layering rule as every adapter: DTOs mirror the venue's JSON exactly
// and a separate mapping step converts to domain types. Numbers stay as
// string_views into the caller's buffer.
//
// Three things differ from okx and binance in ways that reach this file.
//
// Every timestamp is an ISO-8601 STRING, never an epoch number, and the
// fractional part is 3, 6 or 9 digits depending on the endpoint (verified:
// /time sends 3, the ticker channel 6, /products/*/book 9). Nothing is
// converted here; map.cpp owns the parser and explains why it is hand-rolled.
//
// There is no envelope and no numeric error code anywhere on this API. A REST
// failure is a bare {"message":"NotFound"} whose class lives in the HTTP
// status, and a websocket failure is {"type":"error","message":..,"reason":..}
// with no code at all. errors.hpp classifies on those instead.
//
// Frames dispatch on a top-level "type", and the matches channel sends ONE
// print per frame, so there is no data array to walk the way okx needs.

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::coinbase {

enum class WireError : std::uint8_t {
    NotJson,
    MissingField,
    WrongType,
    TooManyEntries,
};

[[nodiscard]] std::string_view to_string(WireError err) noexcept;

// Resolved from the frame's "type" once, so downstream dispatch is an integer
// compare rather than a string compare per message.
enum class EventKind : std::uint8_t {
    Ticker,        // ticker — best bid/ask, republished on every match
    Match,         // match, and the last_match replay sent on subscribe
    Subscriptions, // the subscribe confirmation
    Error,
    Unknown, // heartbeat, status, l2update, snapshot — parsed by nobody here
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view product_id; // empty on control frames
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// {"type":"error","message":"Failed to subscribe","reason":"X is not a valid
// product"}. `reason` carries the detail; `message` alone says nothing about
// which subscription died.
struct StreamError {
    std::string_view message;
    std::string_view reason;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

inline constexpr std::size_t k_max_ack_channels = 16;

struct AckBuffer {
    std::array<std::string_view, k_max_ack_channels> channels{};
};

// The confirmation lists every channel now live on the connection, named by
// the venue rather than by us: a level2_batch request comes back as
// "level2_50" (verified live), so comparing the ack against what was asked
// for would read a confirmed channel as a refused one. An EMPTY array is the
// refusal signal — it is what arrives beside an error frame.
[[nodiscard]] std::expected<std::span<const std::string_view>, WireError>
parse_subscriptions(std::string_view frame, AckBuffer& storage);

// ------------------------------------------------------------ public ----

// The channel also carries price/last_size/side for the match that triggered
// it; those are not read here because the matches channel prints the same
// trade with the side the mapper can actually use (see WireMatch).
struct WireTicker {
    std::string_view product_id;
    std::string_view best_bid;
    std::string_view best_bid_size;
    std::string_view best_ask;
    std::string_view best_ask_size;
    std::string_view time; // ISO-8601
    std::int64_t sequence{0};
    std::int64_t trade_id{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// `side` on this channel is the MAKER's side, so the aggressor took the
// opposite one — hence the field name, which makes the flip in map.cpp
// impossible to read as an identity. VERIFIED live: 21 prints joined to their
// ticker frames by trade_id disagreed on `side` every single time, and every
// print carrying side "sell" landed on the ask, where a taker who bought
// would print. Binance's `m` needs the same care and is the field most often
// read backwards; okx's `side` is already the taker's and needs no flip.
struct WireMatch {
    std::string_view product_id;
    std::string_view price;
    std::string_view size;
    std::string_view time; // ISO-8601
    bool maker_sold{false};
    std::int64_t sequence{0};
    std::int64_t trade_id{0};
};

[[nodiscard]] std::expected<WireMatch, WireError> parse_match(std::string_view frame);

// ------------------------------------------------------------- rest ----

// quote_increment and base_increment are the tick and step THEMSELVES —
// decimal strings like "0.01" — not the count of precision digits bingx and
// mexc publish. Nothing has to be exponentiated to use them, which is what
// makes this mapper shorter than those.
struct WireProduct {
    std::string_view id; // "BTC-USD": hyphenated, unlike every other venue
    std::string_view base_currency;
    std::string_view quote_currency;
    std::string_view quote_increment;
    std::string_view base_increment;
    std::string_view min_market_funds; // quote-currency floor for an order
    std::string_view status;           // online, delisted
    bool trading_disabled{false};
    bool cancel_only{false};
    bool post_only{false};
    bool limit_only{false};
    bool auction_mode{false};
};

// 832 rows live today and the endpoint has no filter, so the ceiling is sized
// for growth; past it a body is a venue incident or an attempt to make us
// allocate.
inline constexpr std::size_t k_max_products = 4096;

struct ProductBuffer {
    std::array<WireProduct, k_max_products> products{};
};

[[nodiscard]] std::expected<std::span<const WireProduct>, WireError>
parse_products(std::string_view body, ProductBuffer& storage);

// /products/{id} answers with the bare object, not a one-element array.
[[nodiscard]] std::expected<WireProduct, WireError> parse_product(std::string_view body);

// Returns /time's `iso` field. Its sibling `epoch` is a JSON floating-point
// number, which is exactly the representation this codebase refuses to make
// decisions on, so the string is the only usable form.
[[nodiscard]] std::expected<std::string_view, WireError> parse_server_time(std::string_view body);

// The whole of a Coinbase REST failure body: {"message":"NotFound"}.
[[nodiscard]] std::expected<std::string_view, WireError> parse_rest_error(std::string_view body);

} // namespace raikiri::venues::coinbase
