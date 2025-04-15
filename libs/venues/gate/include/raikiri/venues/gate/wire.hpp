#pragma once

// Gate.io v4 wire types and parsers.
//
// Same layering rule as every adapter: DTOs mirror Gate's JSON exactly and a
// separate mapping step converts to domain types. Numbers stay as
// string_views into the caller's buffer.
//
// Four things differ from the okx adapter in ways that reach this file.
//
// Gate mixes encodings for the same class of value: prices are JSON strings,
// sizes are JSON INTEGERS, and `order_size_min` is a string in Gate's own SDK
// but an integer on the live endpoint. Numeric fields are therefore read from
// the raw token with the quotes optional.
//
// Timestamps come in both units and the field name does not settle it. `t` is
// milliseconds; `create_time` and `funding_next_apply` are seconds. Worse, the
// REST /futures/{settle}/trades endpoint ships `create_time_ms` as FRACTIONAL
// SECONDS (1786394934.376) where the websocket ships the same field name as
// integer milliseconds — verified live, 2026-08. Only the websocket form is
// parsed here.
//
// The aggressor side is the SIGN of the trade size, not a separate field.
//
// There is no REST envelope: a successful catalogue read is a top-level JSON
// ARRAY, and a refusal is {"label":"CONTRACT_NOT_FOUND",...} whose code is a
// string rather than a number.

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::gate {

enum class WireError : std::uint8_t {
    NotJson,
    MissingField,
    WrongType,
    TooManyEntries,
};

[[nodiscard]] std::string_view to_string(WireError err) noexcept;

// Resolved from the frame's channel and event once, so downstream dispatch is
// an integer compare rather than a string compare per message.
enum class EventKind : std::uint8_t {
    BookTicker, // futures.book_ticker — top of book
    Trade,      // futures.trades
    Ticker,     // futures.tickers — carries funding rate AND mark price
    BookUpdate, // futures.order_book_update — depth deltas
    SubscribeAck,
    Error,
    Pong,
    Unknown,
};

// `contract` is filled only on control frames, where it comes from the echoed
// `payload`. Data frames carry it in four different places depending on the
// channel, so their own parsers report it rather than paying for the descent
// twice.
struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view contract;
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// A refused subscription: the frame keeps event:"subscribe" and adds
// {"error":{"code":2,"message":"unknown currency pair NOPE_USDT"}}. Unlike the
// REST side the code here is a NUMBER. Code 4 nests a REST-style label inside
// the message string; classification still works off the number.
struct StreamError {
    std::int64_t code{0};
    std::string_view message;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// ------------------------------------------------------------ public ----

// Sizes are integer CONTRACT counts, so bid_qty/ask_qty are digit tokens with
// no decimal point. `ts_ms` is milliseconds here even though most other Gate
// epoch fields are seconds.
struct WireBookTicker {
    std::string_view contract;
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::uint64_t update_id{0}; // `u`, exceeds 2^32 in practice
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireBookTicker, WireError> parse_book_ticker(std::string_view frame);

// `qty` is the MAGNITUDE of Gate's signed `size`; the sign became
// aggressor_bought. Reading `size` unsigned loses the taker direction, which
// is how a signed flow series comes out flat.
struct WireTrade {
    std::string_view contract;
    std::string_view qty;
    std::string_view price;
    std::int64_t trade_id{0};
    bool aggressor_bought{false};
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

// One frame can carry several prints, so this returns a span where the other
// channels return one struct.
[[nodiscard]] std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage);

// futures.tickers is the funding channel AND the mark-price channel: one
// subscription carries both, unlike okx. The interval is stated outright, so
// nothing has to be derived from two settlement times.
//
// `funding_rate_indicative` is DEPRECATED per Gate's own SDK ("use
// funding_rate") and was observed equal to it on every frame, so it is
// surfaced but deliberately not mapped to a forecast.
struct WireTicker {
    std::string_view contract;
    std::string_view funding_rate;
    std::string_view funding_rate_indicative;
    std::string_view mark_price;
    std::string_view index_price;
    std::uint64_t funding_next_apply_s{0};
    std::uint32_t funding_interval_s{0};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// A level is {"p":"64156.4","s":35680} on both the websocket and the REST
// snapshot — same object either way, unlike okx's positional arrays.
struct WireLevel {
    std::string_view price;
    std::string_view qty; // "0" means delete this level
};

inline constexpr std::size_t k_max_levels = 512;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// Continuity is the [first_update_id, last_update_id] chain: a delta applies
// when its first_update_id is one past the previous message's last_update_id.
// Gate publishes no snapshot on this channel, so seeding it requires the REST
// order_book read that rest.hpp builds.
struct WireBookUpdate {
    std::string_view contract;
    std::uint64_t first_update_id{0}; // `U`
    std::uint64_t last_update_id{0};  // `u`
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBookUpdate, WireError> parse_book_update(std::string_view frame,
                                                                         LevelBuffer& storage);

// ------------------------------------------------------------- rest ----

// `message` is absent on some refusals (CONTRACT_NOT_FOUND ships the label
// alone), so only the label is required.
struct RestError {
    std::string_view label;
    std::string_view message;
};

// Fails when the body carries no `label`, which is how a success body reports
// itself. Callers gate this on a non-2xx status rather than probing every
// body: the catalogue is over a megabyte and indexing it twice is waste.
[[nodiscard]] std::expected<RestError, WireError> parse_rest_error(std::string_view body);

// From GET /futures/{settle}/contracts. `order_size_min` is a count of
// contracts, and `quanto_multiplier` is how many base units one contract is.
struct WireContract {
    std::string_view name;
    std::string_view status; // prelaunch, trading, delisting, delisted, circuit_breaker
    std::string_view order_price_round;
    std::string_view order_size_min;
    std::string_view quanto_multiplier;
    std::string_view mark_price;
    std::string_view funding_rate;
    std::uint64_t funding_next_apply_s{0};
    std::uint32_t funding_interval_s{0};
    bool in_delisting{false};
};

// 899 live on 2026-08 and the count only grows; past this a response is a
// venue incident or an attempt to make us allocate.
inline constexpr std::size_t k_max_contracts = 2048;

struct ContractBuffer {
    std::array<WireContract, k_max_contracts> contracts{};
};

// The body is a top-level ARRAY, not an object with a data member.
[[nodiscard]] std::expected<std::span<const WireContract>, WireError>
parse_contracts(std::string_view body, ContractBuffer& storage);

// GET /spot/time — milliseconds as a JSON number; futures has no equivalent.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

} // namespace raikiri::venues::gate
