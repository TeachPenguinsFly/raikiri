#pragma once

// XT.com futures wire types and parsers.
//
// `data` is always a single OBJECT, never an array — including on the trade
// channel, which carries exactly one print per frame. There is no data[0] to
// unwrap and no multi-trade buffer.

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::xt {

using WireError = wire::Error;

// Resolved from the frame's `topic` once, so downstream dispatch is an integer
// compare rather than a string compare per message.
enum class EventKind : std::uint8_t {
    Depth,       // depth — top-N snapshot, the BBO and book source
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

[[nodiscard]] std::expected<WireDepth, WireError>
parse_depth(std::string_view frame, LevelBuffer& storage, MarketType market);

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

[[nodiscard]] std::expected<WireTrade, WireError> parse_trade(std::string_view frame,
                                                             MarketType market);

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

// ---------------------------------------------------------- private ----
//
// TWO EXCHANGES UNDER ONE BRAND, and nothing may share a reader between them.
// Futures states an order with long names on BOTH transports (`symbol`,
// `origQty`, `orderSide`, `state`); spot states the same order with long names
// over REST and with TWO-LETTER names over the socket (`s`, `oq`, `sd`, `st`).
// A reader that guessed would read a resting order's price as its quantity.
//
// Numbers arrive in both encodings for the same field — /order/list ships
// `price` as a bare JSON number while the socket quotes it — so every decimal
// below is read with detail::opt_scalar, which keeps the token either way.

// One state, resolved once per record so the mapper branches on an integer.
// PARTIALLY_CANCELED is a cancel that took volume first: the fill was already
// reported by its own trade, so the lifecycle event is a cancel.
enum class WireOrderStatus : std::uint8_t {
    New,
    PartiallyFilled,
    Filled,
    Cancelled,
    Rejected,
    Expired,
    Unknown,
};

[[nodiscard]] WireOrderStatus order_status(std::string_view text) noexcept;

// A page of orders, deals or positions. XT caps `size` at 100 per read, so a
// buffer this size can hold several pages without ever growing.
inline constexpr std::size_t k_max_private_records = 512;

// Futures ---------------------------------------------------------------
//
// `positionSide` is LONG/SHORT and is NOT the direction: a SELL that closes a
// long carries positionSide LONG. `orderSide` is the direction, and it is the
// only field that is.
struct WireFuturesOrder {
    std::string_view symbol;
    std::string_view order_id;
    std::string_view client_order_id;
    std::string_view price;
    std::string_view orig_qty;     // CONTRACTS
    std::string_view executed_qty; // CONTRACTS
    std::string_view avg_price;
    std::string_view order_side;    // BUY | SELL
    std::string_view position_side; // LONG | SHORT
    std::string_view order_type;    // LIMIT | MARKET
    std::string_view time_in_force; // GTC | IOC | FOK | GTX
    WireOrderStatus state{WireOrderStatus::Unknown};
    std::uint64_t created_ms{0};
    std::uint64_t updated_ms{0};
};

struct FuturesOrderBuffer {
    std::array<WireFuturesOrder, k_max_private_records> orders{};
};

// The two transports disagree about liquidity and about the trade id:
// /order/trade-list ships `execId` plus `takerMaker` as TEXT, the socket ships
// neither the id nor that field and says `isMaker` as a BOOLEAN instead. Both
// are read; `liquidity_known` says whether either was present, because a fill
// whose side of the book is guessed makes a maker strategy score itself
// against a number it invented.
struct WireFuturesDeal {
    std::string_view symbol;
    std::string_view exec_id; // empty on the socket push
    std::string_view order_id;
    std::string_view client_order_id;
    std::string_view price;
    std::string_view quantity; // CONTRACTS
    std::string_view fee;
    std::string_view order_side; // empty on /order/trade-list
    bool maker{false};
    bool liquidity_known{false};
    std::uint64_t ts_ms{0};
};

struct FuturesDealBuffer {
    std::array<WireFuturesDeal, k_max_private_records> deals{};
};

// `positionSize` is UNSIGNED; `positionSide` is the only thing that says which
// way a position points.
struct WireFuturesPosition {
    std::string_view symbol;
    std::string_view position_side; // LONG | SHORT
    std::string_view position_size; // CONTRACTS, unsigned
    std::string_view entry_price;
    std::string_view contract_type;
};

struct FuturesPositionBuffer {
    std::array<WireFuturesPosition, k_max_private_records> positions{};
};

// `result` as an ARRAY, or as `result.items` — the paged reads wrap the list
// in a page object and the unpaged ones do not.
[[nodiscard]] std::expected<std::span<const WireFuturesOrder>, WireError>
parse_futures_orders(std::string_view body, FuturesOrderBuffer& storage);

// `data` on the socket push, `result` on the single-order REST read.
[[nodiscard]] std::expected<WireFuturesOrder, WireError>
parse_futures_order(std::string_view body);

[[nodiscard]] std::expected<std::span<const WireFuturesDeal>, WireError>
parse_futures_deals(std::string_view body, FuturesDealBuffer& storage);

[[nodiscard]] std::expected<WireFuturesDeal, WireError> parse_futures_deal(std::string_view frame);

[[nodiscard]] std::expected<std::span<const WireFuturesPosition>, WireError>
parse_futures_positions(std::string_view body, FuturesPositionBuffer& storage);

// The create/cancel/amend answers, whose `result` is the venue order id — a
// BARE STRING on futures create and cancel, an object with `orderId` on the
// futures amend and on every spot write. Empty when the venue named none.
[[nodiscard]] std::expected<std::string_view, WireError>
parse_order_ack(std::string_view body);

// Spot ------------------------------------------------------------------

struct WireSpotOrder {
    std::string_view symbol;
    std::string_view order_id;
    std::string_view client_order_id;
    std::string_view side; // BUY | SELL
    std::string_view type; // LIMIT | MARKET
    std::string_view time_in_force;
    std::string_view price;
    std::string_view orig_qty;
    std::string_view executed_qty;
    std::string_view leaving_qty; // XT states leaves outright; nothing is derived
    std::string_view avg_price;
    std::string_view fee;
    WireOrderStatus state{WireOrderStatus::Unknown};
    std::uint64_t ts_ms{0};
};

struct SpotOrderBuffer {
    std::array<WireSpotOrder, k_max_private_records> orders{};
};

// `takerMaker` is the ACCOUNT's side of the book, and on the socket push it
// arrives as `tm`: 1 taker, 2 maker.
struct WireSpotTrade {
    std::string_view symbol;
    std::string_view trade_id;
    std::string_view order_id;
    std::string_view side; // BUY | SELL; empty on the socket push
    std::string_view price;
    std::string_view quantity;
    std::string_view fee;
    bool maker{false};
    bool liquidity_known{false};
    std::uint64_t ts_ms{0};
};

struct SpotTradeBuffer {
    std::array<WireSpotTrade, k_max_private_records> trades{};
};

// REST: `result` as an array (open-order) or `result.items` (history/trade).
[[nodiscard]] std::expected<std::span<const WireSpotOrder>, WireError>
parse_spot_orders(std::string_view body, SpotOrderBuffer& storage);

[[nodiscard]] std::expected<WireSpotOrder, WireError> parse_spot_order(std::string_view body);

[[nodiscard]] std::expected<std::span<const WireSpotTrade>, WireError>
parse_spot_trades(std::string_view body, SpotTradeBuffer& storage);

// The socket pushes, whose keys are the two-letter set.
[[nodiscard]] std::expected<WireSpotOrder, WireError> parse_spot_order_push(std::string_view frame);

[[nodiscard]] std::expected<WireSpotTrade, WireError> parse_spot_trade_push(std::string_view frame);

// Spot answers {"rc":0,"mc":"SUCCESS","ma":[],"result":...}: `rc` carries one
// bit and `mc` carries the identity of the failure, which is what decides
// retry from halt. Fails rather than reporting success when `rc` is missing.
inline constexpr std::int64_t k_spot_ok = 0;

struct SpotEnvelope {
    std::int64_t rc{0};
    std::string_view mc;
};

[[nodiscard]] std::expected<SpotEnvelope, WireError> parse_spot_envelope(std::string_view body);

// The spot user-stream token, `result.accessToken`, and the futures one,
// whose `result` is the listen key STRING itself.
[[nodiscard]] std::expected<std::string_view, WireError>
parse_spot_ws_token(std::string_view body);

[[nodiscard]] std::expected<std::string_view, WireError> parse_listen_key(std::string_view body);

// /v4/public/time answers `result.serverTime` as a JSON number — an object,
// unlike the futures clock, whose `result` is the number itself.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_spot_server_time(std::string_view body);

// A private-stream frame: {"topic":"order","event":"order@<key>","data":{...}}.
// The topic is what decides the reader; the event repeats it with the listen
// key glued on and carries nothing else.
enum class PrivateTopic : std::uint8_t {
    Order,
    Trade,
    Position,
    Balance,
    Ack,     // {"id":..,"code":0,"msg":"success"} — a subscribe XT accepted
    Refusal, // the same shape with a non-zero code; msg names the failure
    Unknown,
};

struct PrivateFrame {
    PrivateTopic topic{PrivateTopic::Unknown};
    std::int64_t code{0};  // Ack and Refusal only
    std::string_view msg;  // Ack and Refusal only
};

[[nodiscard]] std::expected<PrivateFrame, WireError> parse_private_frame(std::string_view frame);

// Spot catalogue --------------------------------------------------------
//
// XT states spot precision as DIGIT COUNTS, like the futures catalogue, so the
// step is derived the same way. `state` is the text "ONLINE"; the two booleans
// beside it are the real gate — a pair listed and priced for the web UI is not
// necessarily open to the API.
struct WireSpotSymbol {
    std::string_view symbol;
    std::string_view state; // ONLINE | OFFLINE | ...
    std::string_view min_quote_qty; // filters[QUOTE_QTY].min; empty when absent
    std::int64_t price_precision{-1};
    std::int64_t qty_precision{-1};
    bool trading_enabled{false};
    bool openapi_enabled{false};
};

inline constexpr std::size_t k_max_spot_symbols = 4096;

struct SpotSymbolBuffer {
    std::array<WireSpotSymbol, k_max_spot_symbols> symbols{};
};

[[nodiscard]] std::expected<std::span<const WireSpotSymbol>, WireError>
parse_spot_symbols(std::string_view body, SpotSymbolBuffer& storage);

} // namespace raikiri::venues::xt
