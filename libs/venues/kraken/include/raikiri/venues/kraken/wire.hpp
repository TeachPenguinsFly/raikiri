#pragma once

// Kraken Futures wire types and parsers.

#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::kraken {

using WireError = wire::Error;

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

// ------------------------------------------------------------ public ----

// One `ticker` frame carries top of book, mark, index and funding, which is
// why three subscription kinds resolve to this one feed.
//
// funding_rate and relative_funding_rate are NOT interchangeable. The
// absolute one changes units with the contract family, VERIFIED on one
// capture: PF_XBTUSD 0.9455796 / index 64108.28 = 1.4749e-5 ~= its
// relative 1.477885e-5, while PI_XBTUSD 1.39213091e-10 * index = 8.92e-6 ~=
// its relative 8.907125e-6 — reciprocal directions. Only the relative field
// is dimensionless and comparable across venues.
struct WireTicker {
    std::string_view product_id;
    std::string_view bid;
    std::string_view ask;
    std::string_view bid_size;
    std::string_view ask_size;
    std::string_view mark_price;
    std::string_view index;
    std::string_view last;
    std::string_view relative_funding_rate;            // dimensionless; use this
    std::string_view relative_funding_rate_prediction; // next interval forecast
    std::string_view open_interest;
    std::uint64_t next_funding_rate_time_ms{0};
    std::uint64_t time_ms{0};
    bool suspended{false};
};

// Reads both `ticker` and `ticker_lite`; the lite feed leaves the derivative
// fields absent, which is why they are optional here.
[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// `side` is the AGGRESSOR's side — Kraken states the taker direction
// directly, so nothing is inverted here or in the mapper. `type` is "fill"
// for ordinary prints and names a forced event otherwise; a liquidation is
// real volume at a real price, so it is surfaced rather than filtered.
struct WireTrade {
    std::string_view product_id;
    std::string_view uid;
    std::string_view type;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::uint64_t seq{0};
    std::uint64_t time_ms{0};
};

// A `trade_snapshot` replays the last 100 prints on subscribe.
inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

// Reads `trade` (one print at the root) and `trade_snapshot` (a `trades`
// array) through one path, so the caller never branches on which it got.
[[nodiscard]] std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage);

struct WireLevel {
    std::string_view price;
    std::string_view qty; // 0 means delete this level
};

// A live PF_XBTUSD `book_snapshot` carried 1751 bids and 1360 asks: Kraken
// pushes the WHOLE book, not a fixed depth, hence far above okx's 512.
inline constexpr std::size_t k_max_levels = 4096;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// Snapshot and delta are different SHAPES, not one shape with a flag:
// `book_snapshot` carries bids/asks arrays of {price,qty}, a `book` delta is
// a single level at the root with its own `side` and fills exactly one span.
struct WireBook {
    std::string_view product_id;
    bool snapshot{false};
    std::uint64_t seq{0};
    std::uint64_t timestamp_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBook, WireError> parse_book(std::string_view frame,
                                                            LevelBuffer& storage);

// ------------------------------------------------------------- rest ----

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

// `type` does NOT separate perpetual from dated: flexible_futures covers
// both PF_ and FF_, futures_inverse covers both PI_ and FI_. VERIFIED across
// all 298 live rows, `last_trading_time` is present on exactly the dated
// contracts, which is what the mapper keys on. The precision field is signed
// on purpose — see map.cpp.
struct WireInstrument {
    std::string_view symbol;
    std::string_view type;              // flexible_futures | futures_inverse
    std::string_view tick_size;         // raw numeric token
    std::string_view contract_size;     // raw numeric token
    std::string_view underlying;        // inverse families only
    std::string_view last_trading_time; // dated contracts only
    std::int64_t contract_value_trade_precision{0};
    bool tradeable{false};
    bool has_precision{false}; // absent field must not read as precision 0
};

inline constexpr std::size_t k_max_instruments = 1024;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> instruments{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage);

// ---------------------------------------------------------- private ----

// The one-order verdict. sendorder, cancelorder and editorder answer the same
// object under three different keys, so the caller names which it asked for
// rather than the parser guessing from what it finds.
enum class StatusKind : std::uint8_t { Send, Cancel, Edit };

// `status` is Kraken's own vocabulary ("placed", "cancelled", "invalidSize",
// "postWouldExecute", ...) and is the ONLY verdict: the envelope reports
// result "success" for a refused order as readily as for an accepted one.
struct WireOrderStatus {
    std::string_view status;
    std::string_view order_id;
    std::string_view cli_ord_id;
    std::uint64_t received_ms{0};
};

[[nodiscard]] std::expected<WireOrderStatus, WireError> parse_order_status(std::string_view body,
                                                                           StatusKind kind);

// POST /batchorder answers one status per element, in request order, under
// `batchStatus`. A batch whose elements carry no order_tag can be matched only
// positionally, which is why the count matters as much as the contents.
inline constexpr std::size_t k_max_batch = 50;

struct BatchStatusBuffer {
    std::array<WireOrderStatus, k_max_batch> statuses{};
};

[[nodiscard]] std::expected<std::span<const WireOrderStatus>, WireError>
parse_batch_status(std::string_view body, BatchStatusBuffer& storage);

// GET /openorders. `unfilledSize` is what is LEFT and `filledSize` what has
// traded; neither is the original size, which the row never states.
struct WireOpenOrder {
    std::string_view order_id;
    std::string_view cli_ord_id;
    std::string_view symbol;
    std::string_view status;     // untouched | partiallyFilled
    std::string_view order_type; // lmt | stop | take_profit
    std::string_view limit_price;
    std::string_view filled_size;
    std::string_view unfilled_size;
    std::uint64_t last_update_ms{0};
    bool buy{false};
    bool reduce_only{false};
};

inline constexpr std::size_t k_max_open_orders = 1024;

struct OpenOrderBuffer {
    std::array<WireOpenOrder, k_max_open_orders> orders{};
};

[[nodiscard]] std::expected<std::span<const WireOpenOrder>, WireError>
parse_open_orders(std::string_view body, OpenOrderBuffer& storage);

// GET /fills. `fill_time` is kept as TEXT as well as milliseconds: the
// lastFillTime cursor that pages backwards through this endpoint is the very
// string the venue sent, so echoing it avoids re-rendering an instant whose
// fractional width varies frame to frame.
struct WireFill {
    std::string_view fill_id;
    std::string_view order_id;
    std::string_view cli_ord_id;
    std::string_view symbol;
    std::string_view price;
    std::string_view size;
    std::string_view fill_type; // maker | taker | liquidation | ...
    std::string_view fill_time;
    std::uint64_t fill_ms{0};
    bool buy{false};
};

// The endpoint answers at most 100 rows per page; the ceiling is slack.
inline constexpr std::size_t k_max_fills = 256;

struct FillBuffer {
    std::array<WireFill, k_max_fills> fills{};
};

[[nodiscard]] std::expected<std::span<const WireFill>, WireError> parse_fills(std::string_view body,
                                                                              FillBuffer& storage);

// GET /openpositions. `price` is a full-precision average entry that no tick
// size divides — see map.cpp for what that costs.
struct WirePosition {
    std::string_view symbol;
    std::string_view price;
    std::string_view size;
    bool is_long{false};
};

inline constexpr std::size_t k_max_positions = 512;

struct PositionBuffer {
    std::array<WirePosition, k_max_positions> positions{};
};

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view body, PositionBuffer& storage);

// ------------------------------------------------------- private ws ----

struct WireChallenge {
    std::string_view message; // the UUID to sign
};

[[nodiscard]] std::expected<WireChallenge, WireError> parse_challenge(std::string_view frame);

// The open_orders order object.
//
// `qty` is what REMAINS, not what was asked for — VERIFIED against the field's
// own companion `filled`, which is cumulative, and against the venue's
// reference mappings. Reading qty as the original size turns a half-filled
// order into a smaller order that never filled.
struct WireWsOrder {
    std::string_view instrument;
    std::string_view order_id;
    std::string_view cli_ord_id;
    std::string_view type; // limit | stop | take_profit | trailing_stop
    std::string_view remaining_qty;
    std::string_view filled_qty;
    std::string_view limit_price;
    std::string_view stop_price;
    std::uint64_t time_ms{0};
    std::uint64_t last_update_ms{0};
    bool buy{false};
    bool reduce_only{false};
};

// A delta is one of two shapes. A removal carries `order_id` at the root and
// no order object at all, which is why an adapter must remember what it sent:
// nothing in that frame names the instrument.
struct WireOrderDelta {
    std::string_view order_id;
    std::string_view reason;
    bool is_cancel{false};
    bool has_order{false};
    WireWsOrder order{};
};

[[nodiscard]] std::expected<WireOrderDelta, WireError> parse_order_delta(std::string_view frame);

struct OrderSnapshotBuffer {
    std::array<WireWsOrder, k_max_open_orders> orders{};
};

[[nodiscard]] std::expected<std::span<const WireWsOrder>, WireError>
parse_order_snapshot(std::string_view frame, OrderSnapshotBuffer& storage);

// The fills feed. `fee_paid` is signed and denominated in `fee_currency`,
// which is the BASE asset on the inverse (PI_) families and the quote asset on
// the linear (PF_) ones — the same split ContractKind already records.
struct WireWsFill {
    std::string_view instrument;
    std::string_view order_id;
    std::string_view fill_id;
    std::string_view cli_ord_id;
    std::string_view price;
    std::string_view qty;
    std::string_view remaining_order_qty;
    std::string_view fee_paid;
    std::string_view fee_currency;
    std::string_view fill_type; // maker | taker | liquidation | ...
    std::uint64_t time_ms{0};
    bool buy{false};
};

struct WsFillBuffer {
    std::array<WireWsFill, k_max_fills> fills{};
};

// Reads `fills` and `fills_snapshot` through one path: both carry the same
// array under the same key.
[[nodiscard]] std::expected<std::span<const WireWsFill>, WireError>
parse_ws_fills(std::string_view frame, WsFillBuffer& storage);

// Epoch milliseconds from an ISO-8601 UTC instant. Exposed because Kraken
// mixes both encodings and a caller must never guess which it holds.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_iso8601_ms(std::string_view text);

} // namespace raikiri::venues::kraken
