#pragma once

// MEXC futures (contract.mexc.com) wire types and parsers.
//
// There is no `arg` object to peel. A data frame is
// {"channel":"push.ticker","symbol":"BTC_USDT","data":{...},"ts":...} and both
// the channel and the symbol sit at the ENVELOPE level, `data` being the
// payload alone. VERIFIED live for ticker, deal, depth and depth.full.
//
// A refused subscription is {"channel":"rs.error","data":"<text>"} — the
// reason is a human-readable string and there is NO code anywhere in the
// frame. An unknown `method` gets no answer at all (verified: sub.bogus was
// silently dropped while the two malformed sub.ticker calls each drew an
// rs.error), so silence is not evidence a channel was accepted.

#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::mexc {

using WireError = wire::Error;

// Resolved from the envelope `channel` once, so downstream dispatch is an
// integer compare rather than a string compare per message.
enum class EventKind : std::uint8_t {
    Ticker,       // push.ticker — bid1/ask1, fair price and funding rate
    Trade,        // push.deal
    Depth,        // push.depth — incremental
    DepthFull,    // push.depth.full — limit-N snapshot
    SubscribeAck, // rs.sub.*
    Pong,         // answer to {"method":"ping"}
    Error,        // rs.error

    // Private, and only after a successful login on the same socket. MEXC
    // pushes these unbidden — there is no sub.personal.* to send, so silence
    // after login is indistinguishable from an account with nothing to say.
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

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// No code field exists to carry: classification has nothing to key on, so the
// caller records the text and counts the refusal.
struct StreamError {
    std::string_view msg;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// ------------------------------------------------------------ public ----

// bid1/ask1 are PRICES ONLY — MEXC publishes no top-of-book sizes on this
// channel, which is why the BBO is built from depth.full instead. What is here
// and nowhere else on the public socket is fundingRate, so this doubles as the
// funding stream. fair_price is MEXC's mark: it is the price liquidation and
// unrealised PnL are computed against.
struct WireTicker {
    std::string_view symbol;
    std::string_view bid_price;
    std::string_view ask_price;
    std::string_view last_price;
    std::string_view fair_price;
    std::string_view index_price;
    std::string_view funding_rate;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// `aggressor_bought` comes from `T`, which MEXC documents as the taker
// direction: 1 buy, 2 sell. Read the right way round here, so neither this nor
// the mapper inverts anything — binance's `m` says the opposite thing and
// reading the two the same way is how a signed flow series comes out backwards.
struct WireTrade {
    std::string_view symbol;
    std::string_view trade_id;
    std::string_view price;
    std::string_view qty; // CONTRACTS, not base units
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

struct WireLevel {
    std::string_view price;
    std::string_view qty; // "0" means delete this level
};

// depth.full offers 200 a side and the widest incremental frame observed
// carried 33; the ceiling is headroom against a resync burst rather than a
// measured maximum, and past it a frame is an attempt to make us allocate.
inline constexpr std::size_t k_max_levels = 1024;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// One parser for both depth channels: the payloads are the same object, and
// `snapshot` records which channel delivered it. That distinction is load
// bearing — a push.depth delta is a set of CHANGED levels in no particular
// order (verified: a BTC_USDT delta led with a bid 1900 away from the touch),
// so only a snapshot's first entry is the touch.
//
// Continuity on the incremental channel is the version chain: a delta applies
// when its `begin` follows the previous frame's `end`. Snapshots carry
// `version` alone.
struct WireDepth {
    std::string_view symbol;
    bool snapshot{false};
    std::int64_t version{0};
    std::int64_t begin{0};
    std::int64_t end{0};
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireDepth, WireError> parse_depth(std::string_view frame,
                                                              LevelBuffer& storage);

// ------------------------------------------------------------- rest ----

inline constexpr std::int64_t k_ok = 0;

struct RestEnvelope {
    bool success{false};
    std::int64_t code{0};
    std::string_view msg;
};

// Both halves are required: an error body carries success:false and a nonzero
// code, and a body whose envelope cannot be read must never be mistaken for
// one that said success.
[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

// MEXC states precision as SCALES — counts of digits — never as steps. The
// derivation to a step lives in the mapper; the wire layer keeps the digit
// counts MEXC actually sent.
struct WireInstrument {
    std::string_view symbol;
    std::string_view base_coin; // the futures catalogue names both halves
    std::string_view quote_coin;
    std::int64_t state{-1}; // 0 = enabled
    std::int64_t price_scale{-1};
    std::int64_t vol_scale{-1};
    std::string_view contract_size; // base units per contract
    std::string_view min_vol;       // contracts
    bool api_allowed{false};
};

// /contract/detail returned 1086 rows on 2026-08-10 and grows with every
// listing; past the ceiling a response is a venue incident or an attempt to
// make us allocate.
inline constexpr std::size_t k_max_instruments = 4096;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> instruments{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage);

// /contract/ping, whose `data` is the epoch-millisecond number itself.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// GET /contract/funding_rate/{symbol}. The websocket carries a rate but no
// settlement boundary at all, so this is the only place the schedule exists.
// collect_cycle_h is stated in HOURS.
struct WireFundingRate {
    std::string_view symbol;
    std::string_view funding_rate;
    std::string_view fair_price;
    std::uint64_t next_settle_ms{0};
    std::int64_t collect_cycle_h{0};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireFundingRate, WireError> parse_funding_rate(std::string_view body);

// ---------------------------------------------------------- private ----
//
// Two disjoint schemas live here, and nothing may share a parser between them.
// Futures state an order with numeric fields (`vol`, `dealVol`, integer
// `side`, integer `state`); spot states the same order the binance way with
// quoted decimals and SCREAMING_SNAKE statuses. A reader that guesses reads a
// close order as an open one.

// Resolved once per record so the mapper branches on an integer. MEXC futures
// collapses New and PartiallyFilled into state 2 and only the filled volume
// tells them apart, which is why that split happens in the mapper and not
// here.
enum class WireOrderStatus : std::uint8_t {
    New,
    PartiallyFilled,
    Filled,
    Cancelled,
    Rejected,
    Expired,
    Unknown,
};

[[nodiscard]] WireOrderStatus spot_order_status(std::string_view text) noexcept;

// One private record is one order; a REST list can carry a page of them.
inline constexpr std::size_t k_max_private_records = 512;

// Futures --------------------------------------------------------------

// `order_id` reads `orderId` on REST and `id` on the socket — the same value
// under two names, which is the one place the two futures schemas disagree.
// `side` is the four-valued OPEN/CLOSE enum, not a direction: 1 open long,
// 2 close short, 3 open short, 4 close long.
struct WireFuturesOrder {
    std::string_view symbol;
    std::string_view order_id;
    std::string_view external_oid; // our ULID, echoed back
    std::string_view price;
    std::string_view vol;      // CONTRACTS ordered
    std::string_view deal_vol; // CONTRACTS filled so far
    std::string_view deal_avg_price;
    std::int64_t side{0};
    std::int64_t order_type{0};
    std::int64_t state{0};      // 1 pending, 2 working, 3 filled, 4 cancelled, 5 invalid
    std::int64_t error_code{0}; // non-zero on a venue-side refusal
    std::uint64_t update_ms{0};
};

struct FuturesOrderBuffer {
    std::array<WireFuturesOrder, k_max_private_records> orders{};
};

// `taker` on REST, `isTaker` on the socket — read both, because a fill with
// the liquidity flag lost is a maker strategy with no way to score itself.
struct WireFuturesDeal {
    std::string_view symbol;
    std::string_view deal_id;
    std::string_view order_id;
    std::string_view price;
    std::string_view vol; // CONTRACTS
    std::string_view fee;
    std::int64_t side{0};
    bool taker{false};
    bool liquidity_known{false};
    std::uint64_t ts_ms{0};
};

struct FuturesDealBuffer {
    std::array<WireFuturesDeal, k_max_private_records> deals{};
};

// `position_type` is 1 long / 2 short and `hold_vol` is UNSIGNED, so the sign
// of a net position lives in the type field alone.
struct WireFuturesPosition {
    std::string_view symbol;
    std::string_view hold_vol;
    std::string_view hold_avg_price;
    std::int64_t position_type{0};
    std::int64_t state{0}; // 1 holding, 2 system auto-holding, 3 closed
    std::uint64_t update_ms{0};
};

struct FuturesPositionBuffer {
    std::array<WireFuturesPosition, k_max_private_records> positions{};
};

// `data` as an ARRAY (the REST list endpoints).
[[nodiscard]] std::expected<std::span<const WireFuturesOrder>, WireError>
parse_futures_orders(std::string_view body, FuturesOrderBuffer& storage);

// `data` as a single OBJECT: the socket push, and the REST single-order reads.
[[nodiscard]] std::expected<WireFuturesOrder, WireError> parse_futures_order(std::string_view body);

[[nodiscard]] std::expected<std::span<const WireFuturesDeal>, WireError>
parse_futures_deals(std::string_view body, FuturesDealBuffer& storage);

[[nodiscard]] std::expected<WireFuturesDeal, WireError> parse_futures_deal(std::string_view frame);

[[nodiscard]] std::expected<std::span<const WireFuturesPosition>, WireError>
parse_futures_positions(std::string_view body, FuturesPositionBuffer& storage);

[[nodiscard]] std::expected<WireFuturesPosition, WireError>
parse_futures_position(std::string_view frame);

// Spot -----------------------------------------------------------------

struct WireSpotOrder {
    std::string_view symbol;
    std::string_view order_id;
    std::string_view client_order_id;
    std::string_view price;
    std::string_view orig_qty;
    std::string_view executed_qty;
    std::string_view side; // BUY | SELL
    std::string_view type; // LIMIT | MARKET | LIMIT_MAKER
    WireOrderStatus status{WireOrderStatus::Unknown};
    std::uint64_t update_ms{0};
};

struct SpotOrderBuffer {
    std::array<WireSpotOrder, k_max_private_records> orders{};
};

// `is_buyer` is the ACCOUNT's side, not the aggressor's — the opposite of what
// the public deal channel's flag means, and reading it that way inverts the
// side of every reconciled fill.
struct WireSpotTrade {
    std::string_view symbol;
    std::string_view trade_id;
    std::string_view order_id;
    std::string_view client_order_id;
    std::string_view price;
    std::string_view qty;
    std::string_view commission;
    bool is_buyer{false};
    bool is_maker{false};
    std::uint64_t ts_ms{0};
};

struct SpotTradeBuffer {
    std::array<WireSpotTrade, k_max_private_records> trades{};
};

// The spot bodies are BARE arrays and objects: there is no envelope to peel,
// which is the second reason spot and futures cannot share a reader.
[[nodiscard]] std::expected<std::span<const WireSpotOrder>, WireError>
parse_spot_orders(std::string_view body, SpotOrderBuffer& storage);

[[nodiscard]] std::expected<WireSpotOrder, WireError> parse_spot_order(std::string_view body);

[[nodiscard]] std::expected<std::span<const WireSpotTrade>, WireError>
parse_spot_trades(std::string_view body, SpotTradeBuffer& storage);

// Spot refusals are {"code":10072,"msg":"..."} and successes carry no `code`
// at all, so an absent code is success rather than an unreadable body.
struct SpotError {
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<SpotError, WireError> parse_spot_error(std::string_view body);

// Spot catalogue -------------------------------------------------------

// MEXC spot states precision as a SCALE for price (`quotePrecision`) and as a
// STEP for quantity (`baseSizePrecision`) in the same row — the two arrive in
// different forms and the mapper treats them differently. There is no
// PRICE_FILTER or LOT_SIZE in `filters`; the only filter BTCUSDT carried on
// 2026-08-26 was PERCENT_PRICE_BY_SIDE.
struct WireSpotSymbol {
    std::string_view symbol;
    std::string_view base_asset; // the spot symbol `BTCUSDT` states no pair
    std::string_view quote_asset;
    std::string_view status;          // "1" is trading, as a STRING
    std::string_view base_size_step;  // baseSizePrecision, a decimal step
    std::int64_t quote_precision{-1}; // price decimals
    bool spot_trading_allowed{false};
};

inline constexpr std::size_t k_max_spot_symbols = 8192;

struct SpotSymbolBuffer {
    std::array<WireSpotSymbol, k_max_spot_symbols> symbols{};
};

[[nodiscard]] std::expected<std::span<const WireSpotSymbol>, WireError>
parse_spot_symbols(std::string_view body, SpotSymbolBuffer& storage);

// /api/v3/time, whose body is {"serverTime":<epoch ms>}.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_spot_server_time(std::string_view body);

} // namespace raikiri::venues::mexc
