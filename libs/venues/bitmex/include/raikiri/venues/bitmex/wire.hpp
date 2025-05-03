#pragma once

// BitMEX v1 wire types and parsers.
//
// A data frame is {"table":..,"action":"partial|insert|update|delete","data":[]}
// and `update` carries ONLY the columns that changed. Every field below except
// the one identifying the row is therefore OPTIONAL here — an empty view means
// ABSENT, never zero — and the mapper decides what is publishable. Requiring
// markPrice would reject most of the `instrument` stream; requiring both sides
// would reject a fifth of `quote` (both verified live).
//
// `quote` rows are per liquidity POOL, and the aggregate is their sum at the
// best price — verified against the Aggregated orderBookL2_25 top, where
// 80200 + 2043500 is the 2123700 the venue reports at 64054.7.

#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitmex {

using WireError = wire::Error;

// Resolved from `table` once so dispatch is an integer compare.
enum class TableKind : std::uint8_t {
    Quote,      // top of book, one row per pool
    Trade,      // prints
    Instrument, // mark price, funding, index — and the whole catalogue row
    OrderBook,  // orderBookL2_25
    SubscribeAck,
    Error,
    Info, // the connection banner
    Unknown,
};

// `partial` is BitMEX's word for a snapshot: it opens every subscription,
// re-opens it after a reconnect, and is the only action carrying every column.
enum class FrameAction : std::uint8_t { Partial, Insert, Update, Delete, None };

struct FrameInfo {
    TableKind table{TableKind::Unknown};
    FrameAction action{FrameAction::None};
};

// The keepalive answer is the bare text "pong", not JSON, so it lands here as
// NotJson and the caller ignores it — the same path as any frame BitMEX adds.
[[nodiscard]] std::expected<FrameInfo, WireError> parse_frame(std::string_view frame);

// {"status":400,"error":"Unknown or expired symbol: X",...}. Surfaced rather
// than dropped: a subscription that was never established and one that is
// merely quiet look identical downstream.
struct StreamError {
    std::int64_t status{0};
    std::string_view message;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// One ack per arg, not one per subscribe message.
struct SubscribeAck {
    std::string_view topic;
    bool success{false};
};

[[nodiscard]] std::expected<SubscribeAck, WireError> parse_subscribe_ack(std::string_view frame);

// ------------------------------------------------------------ public ----

// One row of `quote`. A side is present as a price/size PAIR or not at all.
struct WireQuote {
    std::string_view symbol;
    std::string_view pool; // "Primary" / "Secondary"
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::string_view ts; // ISO-8601
};

// A `partial` carries one row per pool; two pools exist today.
inline constexpr std::size_t k_max_quotes = 16;

struct QuoteBuffer {
    std::array<WireQuote, k_max_quotes> quotes{};
};

[[nodiscard]] std::expected<std::span<const WireQuote>, WireError>
parse_quotes(std::string_view frame, QuoteBuffer& storage);

// `side` is the AGGRESSOR's, stated as "Buy"/"Sell" — BitMEX names the taker
// directly, so nothing is inverted here or in the mapper. Binance's `m` says
// the opposite thing; reading the two the same way is how a signed flow series
// comes out backwards.
struct WireTrade {
    std::string_view symbol;
    std::string_view trade_id;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::string_view ts;
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

// One frame can carry several prints, so this returns a span where the
// single-row tables return one struct.
[[nodiscard]] std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage);

// A row of `instrument`. Only `symbol` is required: the common update carries
// three columns out of eighty.
//
// funding_ts is the NEXT settlement the current rate applies to, and
// funding_interval is a DURATION that BitMEX encodes as a timestamp — see
// map.hpp, where it is decoded.
struct WireInstrumentTick {
    std::string_view symbol;
    std::string_view mark_price;
    std::string_view funding_rate;
    std::string_view indicative_funding_rate;
    std::string_view funding_ts;
    std::string_view funding_interval;
    std::string_view ts;
};

// Reads data[0]: the subscription is always symbol-scoped, so a frame never
// carries a second instrument.
[[nodiscard]] std::expected<WireInstrumentTick, WireError>
parse_instrument_tick(std::string_view frame);

// `id` is the venue's level handle and the key a `delete` names; a delete
// carries no size at all, hence qty optional like everything else.
struct WireLevel {
    std::string_view symbol;
    std::int64_t id{0};
    bool bid{false};
    std::string_view price;
    std::string_view qty;
};

// The snapshot is 25 a side; the ceiling leaves room without letting a hostile
// frame make us allocate.
inline constexpr std::size_t k_max_levels = 128;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> levels{};
};

[[nodiscard]] std::expected<std::span<const WireLevel>, WireError>
parse_book(std::string_view frame, LevelBuffer& storage);

// ------------------------------------------------------------- rest ----

// The only error body BitMEX has. `name` is coarse — "HTTPError" covers both
// 403 and 404 — so classification leans on the HTTP status first.
struct RestError {
    std::string_view message;
    std::string_view name;
};

[[nodiscard]] std::expected<RestError, WireError> parse_rest_error(std::string_view body);

// A row of GET /instrument/active, whose name is a lie: it returns Unlisted and
// Settled rows too (16 of 66 on capture day), so `state` decides, not the URL.
// The three scale fields are mutually exclusive by contract type, and which one
// is present is how BitMEX says inverse, linear or spot. map.cpp reads them.
struct WireInstrument {
    std::string_view symbol;
    std::string_view state; // "Open" / "Unlisted" / "Settled"
    std::string_view typ;   // FFWCSX perp, FFSCSX perp, FFCCSX dated, IFXXXP spot
    std::string_view tick_size;
    std::string_view lot_size;
    std::string_view multiplier;
    std::string_view underlying_to_settle;   // inverse rows
    std::string_view quote_to_settle;        // linear rows
    std::string_view underlying_to_position; // spot rows
    std::string_view expiry;                 // dated rows
    std::string_view funding_interval;
    std::string_view maker_fee;
    std::string_view taker_fee;
    bool inverse{false};
    bool quanto{false};
};

inline constexpr std::size_t k_max_instruments = 512;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> instruments{};
};

// The body is a bare JSON array; there is no envelope to unwrap first.
[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage);

} // namespace raikiri::venues::bitmex
