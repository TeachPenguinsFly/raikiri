#pragma once

// Kraken Futures wire types and parsers.

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::kraken {

enum class WireError : std::uint8_t {
    NotJson,
    MissingField,
    WrongType,
    TooManyEntries,
};

[[nodiscard]] std::string_view to_string(WireError err) noexcept;

// Resolved from `feed` (or `event`) once, so dispatch is an integer compare.
enum class EventKind : std::uint8_t {
    Ticker,     // ticker — bid/ask/mark/index/funding in one frame
    TickerLite, // ticker_lite — the same frame with the derivative fields cut
    Trade,      // trade and trade_snapshot
    Book,       // book and book_snapshot
    Subscribed, // subscribed / unsubscribed acknowledgement
    Alert,      // a refusal; see StreamAlert
    Info,       // {"event":"info","version":1} on connect
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

// Epoch milliseconds from an ISO-8601 UTC instant. Exposed because Kraken
// mixes both encodings and a caller must never guess which it holds.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_iso8601_ms(std::string_view text);

} // namespace raikiri::venues::kraken
