#pragma once

// XT.com futures wire types and parsers.
//
// `data` is always a single OBJECT, never an array — including on the trade
// channel, which carries exactly one print per frame. There is no data[0] to
// unwrap and no multi-trade buffer.

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::xt {

enum class WireError : std::uint8_t {
    NotJson,
    MissingField,
    WrongType,
    TooManyEntries,
};

[[nodiscard]] std::string_view to_string(WireError err) noexcept;

// Resolved from the frame's `topic` once, so downstream dispatch is an integer
// compare rather than a string compare per message.
enum class EventKind : std::uint8_t {
    Depth,       // depth — top-N snapshot, the BBO source
    DepthUpdate, // depth_update — incremental deltas
    Trade,       // trade
    MarkPrice,   // mark_price
    Ack,         // {"code":0,...} — a subscription XT accepted
    Error,       // {"code":1,...} — a subscription XT refused
    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view symbol; // empty on control frames
};

// XT answers a bad `method` with the bare text "Invalid parameter", not JSON,
// so that fails here and the caller ignores it — the same path as any frame XT
// adds later.
[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// A control frame. `code` is 0 on an accepted subscription and 1 on every
// refusal XT has been seen to produce; `msg` is the only thing that
// distinguishes one failure from another, so it is surfaced verbatim.
struct StreamAck {
    std::int64_t code{0};
    std::string_view msg;
    std::string_view id; // echoed from the subscribe frame; absent on some errors
};

[[nodiscard]] std::expected<StreamAck, WireError> parse_stream_ack(std::string_view frame);

// ------------------------------------------------------------ public ----

struct WireLevel {
    std::string_view price;
    std::string_view qty; // "0" means delete this level on depth_update
};

// `depth` tops out at 50 levels a side and depth_update deltas have been seen
// at ten; the ceiling leaves room without letting a hostile frame make us
// allocate.
inline constexpr std::size_t k_max_levels = 128;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// `depth` is a self-contained top-N snapshot on a fixed cadence, which is what
// makes a BBO available without maintaining a book. `id` is XT's update id as
// a digit string.
struct WireDepth {
    std::string_view symbol;
    std::uint64_t update_id{0};
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireDepth, WireError> parse_depth(std::string_view frame,
                                                              LevelBuffer& storage);

// Continuity on depth_update is the u/pu chain: a delta applies when its
// prev_update_id equals the previous message's update_id. `fu` is the first
// id folded into the delta and is always pu + 1 in every frame captured, so it
// carries no information the pair does not already give.
struct WireDepthUpdate {
    std::string_view symbol;
    std::uint64_t first_update_id{0};
    std::uint64_t prev_update_id{0};
    std::uint64_t update_id{0};
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireDepthUpdate, WireError> parse_depth_update(std::string_view frame,
                                                                           LevelBuffer& storage);

// `m` is the AGGRESSOR's side, not the maker's: VERIFIED against a 100 ms book
// over 47 prints — "ASK" only ever printed at or below the best bid and "BID"
// only ever at or above the best ask, never the reverse. Reading it as a maker
// flag inverts every signed-flow series built on it.
//
// `a` is a count of CONTRACTS, like every quantity XT publishes.
struct WireTrade {
    std::string_view symbol;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireTrade, WireError> parse_trade(std::string_view frame);

struct WireMarkPrice {
    std::string_view symbol;
    std::string_view mark_price;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view frame);

// ------------------------------------------------------------- rest ----

// XT publishes no funding channel at all — `funding_rate@`, `funding@` and
// `fundingRate@` are each refused with "No enum constant ... Topic.x", so this
// arrives only by polling /public/q/funding-rate.
//
// rate_token is the RAW JSON token because XT ships fundingRate as a NUMBER
// rather than the digit string every other decimal here arrives as. It is
// plain decimal notation padded to a fixed scale ("-0.000002000000000000" —
// 18 fractional digits, all but six of them zero), which parse_decimal_e9
// accepts; 60 symbols were sampled and none used an exponent.
//
// interval_h is `collectionInternal` — XT's spelling — and its unit is HOURS.
struct WireFundingRate {
    std::string_view symbol;
    std::string_view rate_token;
    std::uint64_t next_collection_ms{0};
    std::int64_t interval_h{0};
};

[[nodiscard]] std::expected<WireFundingRate, WireError> parse_funding_rate(std::string_view body);

inline constexpr std::int64_t k_ok = 0;

// `code` is empty when the call succeeded. Both halves matter: returnCode says
// whether to look, error.code says what happened.
struct RestEnvelope {
    std::int64_t return_code{0};
    std::string_view code;
    std::string_view msg;
};

// Fails rather than reporting success when returnCode is missing or is not a
// number: a body whose envelope cannot be read must never be mistaken for a
// body that said 0.
[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

// XT states the price step outright (minStepPrice) but only a digit count for
// quantity, so the two arrive in different forms and the mapper treats them
// differently. state/precision/the switches are JSON numbers and booleans.
//
// The switches are the real tradability gate: `state` was 0 on all 1067 rows
// in the live catalogue, while tradeSwitch and isOpenApi split it roughly in
// half. A row that is not open to the API cannot be traded by this system even
// though XT still lists and prices it.
struct WireInstrument {
    std::string_view symbol;
    std::string_view product_type; // "perpetual" | "futures"
    std::string_view tick_size;    // minStepPrice
    std::string_view contract_size;
    std::string_view min_notional;
    std::int64_t state{-1};
    std::int64_t qty_precision{-1};
    bool trade_switch{false};
    bool open_api{false};
};

inline constexpr std::size_t k_max_instruments = 2048;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> instruments{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage);

// /public/time answers with the epoch milliseconds as a bare number in
// `result`, not an object.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

} // namespace raikiri::venues::xt
