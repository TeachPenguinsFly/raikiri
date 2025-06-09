#pragma once

// BingX USDT-perpetual wire types and parsers.
//
// The keepalive is the literal text "Ping" after inflation and must be answered
// with "Pong". It is not JSON, so parse_event resolves it before opening a
// document.

#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bingx {

using WireError = wire::Error;

// Resolved from `dataType` once, so dispatch is an integer compare per message.
enum class EventKind : std::uint8_t {
    BookTicker, // {sym}@bookTicker — top of book, one object
    Trade,      // {sym}@trade
    Depth,      // {sym}@depth100@100ms — a 100-level snapshot, not a delta
    MarkPrice,  // {sym}@markPrice — mark only; BingX streams no funding
    Ping,       // the bare text "Ping"
    ControlAck, // a subscribe/unsubscribe answer with code 0
    Error,      // the same shape with a non-zero code
    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view symbol; // empty on control frames and on Ping
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// The keepalive, before any JSON parser sees it. parse_event resolves it for
// the public socket; the user streams need it too and have no dataType to
// dispatch on — and a socket that stops answering "Pong" is closed.
[[nodiscard]] bool is_ping(std::string_view frame) noexcept;

// The answer to a sub/unsub, echoing the id that asked. A refusal is the same
// shape with a non-zero code, so the two are one field apart — and a refusal
// read as an acknowledgement looks exactly like a stream that is merely quiet.
struct ControlFrame {
    std::string_view id;
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<ControlFrame, WireError> parse_control(std::string_view frame);

// ------------------------------------------------------------ public ----

// `T` is when the book changed, `E` when BingX sent it — the only latency
// measurement this channel offers.
struct WireBookTicker {
    std::string_view symbol;
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::uint64_t update_id{0};
    std::uint64_t transaction_ms{0};
    std::uint64_t event_ms{0};
};

[[nodiscard]] std::expected<WireBookTicker, WireError> parse_book_ticker(std::string_view frame);

// `m` says whether the BUYER was the maker, so the aggressor is the opposite
// side — the inverse of okx's `side`, which names the taker directly. VERIFIED
// against 166 live prints matched to the concurrent bookTicker: 163 agreed, 3
// disagreed on a quote that had already moved. Reading the two venues the same
// way is how a signed flow series comes out backwards. There is no trade id.
struct WireTrade {
    std::string_view symbol;
    std::string_view price;
    std::string_view qty;
    std::uint64_t trade_ms{0};
    bool buyer_is_maker{false};
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

// One frame can carry several prints, hence a span.
[[nodiscard]] std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage);

struct WireMarkPrice {
    std::string_view symbol;
    std::string_view mark_price;
    std::uint64_t event_ms{0};
};

[[nodiscard]] std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view frame);

struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

// The depth channels top out at 100 levels a side, and the ceiling leaves room
// without letting a hostile frame make us allocate.
inline constexpr std::size_t k_max_levels = 128;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// A complete snapshot of the top N levels with NO sequence number and no delta
// chain, so there is no continuity to validate. `ts` sits beside `data`, and
// the payload never names the symbol — that comes from `dataType`.
struct WireDepth {
    std::string_view symbol;
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids; // descending
    std::span<const WireLevel> asks; // ascending
};

[[nodiscard]] std::expected<WireDepth, WireError> parse_depth(std::string_view frame,
                                                              LevelBuffer& storage);

// ------------------------------------------------------------- rest ----

inline constexpr std::int64_t k_ok = 0;

struct RestEnvelope {
    std::int64_t code{0};
    std::string_view msg;
};

// Every REST body is {"code":<number>,"msg":...,"data":...}, but a failure
// drops `data` for `timestamp`, so only `code` and `msg` can be relied on.
[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

// BingX states PRECISIONS — counts of decimal digits — where every other venue
// here states a tick and a lot size; map.cpp derives the steps from them.
//
// `size` looks like a quantity step and is not one: it matches 10^-quantity
// _precision on 1017 of 1021 live rows and disagrees on the rest (SOL-USDT says
// "1" at a precision of 2, and live SOL quantities carry two decimals). Carried
// so a row that contradicts the precision is visible; nothing maps it.
//
// The minimums are raw JSON tokens because BingX emits them as NUMBERS, and
// tradeMinQuantity sometimes in scientific notation ("7e-05"), which
// parse_decimal_e9 refuses — better than failing a catalogue over formatting.
struct WireContract {
    std::string_view symbol;
    std::string_view size;
    std::string_view trade_min_quantity;
    std::string_view trade_min_usdt;
    std::int64_t status{0}; // 1 = online; 25 = a listed-but-not-trading row
    std::int64_t price_precision{0};
    std::int64_t quantity_precision{0};
};

inline constexpr std::size_t k_max_instruments = 2048; // 1021 rows live today

struct ContractBuffer {
    std::array<WireContract, k_max_instruments> contracts{};
};

[[nodiscard]] std::expected<std::span<const WireContract>, WireError>
parse_contracts(std::string_view body, ContractBuffer& storage);

// /quote/premiumIndex is the ONLY funding source BingX has: @fundingRate,
// @premiumIndex and @markPriceAndFundingRate are each refused with 80015
// (VERIFIED), so funding is a poll. `last_funding_rate` is the rate in force
// for the NEXT settlement, not one already charged.
struct WirePremiumIndex {
    std::string_view symbol;
    std::string_view mark_price;
    std::string_view index_price;
    std::string_view last_funding_rate;
    std::uint64_t next_funding_ms{0};
    std::int64_t funding_interval_hours{0};
};

[[nodiscard]] std::expected<WirePremiumIndex, WireError> parse_premium_index(std::string_view body);

[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// ------------------------------------------------------ spot catalogue ----

// /openApi/spot/v1/common/symbols states a tickSize and a stepSize outright,
// where the swap tree states digit counts and map.cpp derives the steps. Both
// arrive as JSON NUMBERS and often in scientific notation ("1e-06", and 1e-12
// on nine live rows), so they are carried as raw tokens and read by the
// exponent-aware parser; a step finer than 1e-9 has no exact FixedE9 and the
// row is refused rather than rounded into a grid the venue does not use.
//
// `status` is 1 for online and 0, 10 or 25 otherwise (VERIFIED across 2258
// live rows). `apiStateBuy`/`apiStateSell` are a SECOND gate: 36 of the 705
// online pairs are closed to API orders in at least one direction.
struct WireSpotSymbol {
    std::string_view symbol;
    std::string_view tick_size;
    std::string_view step_size;
    std::string_view min_notional;
    std::int64_t status{0};
    bool api_buy{false};
    bool api_sell{false};
};

inline constexpr std::size_t k_max_spot_symbols = 4096; // 2258 rows live today

struct SpotSymbolBuffer {
    std::array<WireSpotSymbol, k_max_spot_symbols> symbols{};
};

[[nodiscard]] std::expected<std::span<const WireSpotSymbol>, WireError>
parse_spot_symbols(std::string_view body, SpotSymbolBuffer& storage);

// ---------------------------------------------------------- private ----

// BingX spells a resting order PENDING on both trees and a cancelled one
// CANCELED on one and CANCELLED on the other (VERIFIED in BingX's own worked
// examples), and its coin-M push has been seen answering "Unknown". Reading
// any of those as a status we recognise is how an order that is still working
// gets adopted as finished.
enum class WireOrderStatus : std::uint8_t {
    New, // NEW and PENDING: accepted, nothing filled
    PartiallyFilled,
    Filled,
    Canceled,
    Expired,
    Rejected, // spot's FAILED
    Unknown,
};

[[nodiscard]] WireOrderStatus order_status_of(std::string_view text) noexcept;

// One row type for both trees. The fields they share are the ones an order
// report is made of; the ones only the perpetual tree states (avgPrice,
// positionSide) are optional here rather than a second struct.
//
// `order_id` is a raw token because BingX mints ids past 2^53 and emits them
// as JSON numbers on one endpoint and quoted strings on another.
struct WireOrder {
    std::string_view symbol;
    std::string_view order_id;
    std::string_view client_order_id;
    std::string_view side; // BUY/SELL
    std::string_view type; // LIMIT/MARKET/...
    std::string_view price;
    std::string_view avg_price;
    std::string_view orig_qty;
    std::string_view executed_qty;
    WireOrderStatus status{WireOrderStatus::Unknown};
    std::uint64_t update_ms{0};
};

inline constexpr std::size_t k_max_orders = 1024;

struct OrderBuffer {
    std::array<WireOrder, k_max_orders> orders{};
};

// data.orders[] on both trees.
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_orders(std::string_view body, OrderBuffer& storage);

// The answer to a create or a cancel. Spot puts the order in `data` and the
// perpetual tree wraps it one level deeper in `data.order`; this reads either,
// because the difference is a routing detail and not a schema this adapter
// wants two parsers for.
[[nodiscard]] std::expected<WireOrder, WireError> parse_order_ack(std::string_view body);

// `position_amt` is UNSIGNED and `position_side` carries the sign. A long and
// a short of the same size are the same number here.
struct WirePosition {
    std::string_view symbol;
    std::string_view position_side; // LONG/SHORT
    std::string_view position_amt;
    std::string_view avg_price;
    std::uint64_t update_ms{0};
};

inline constexpr std::size_t k_max_positions = 512;

struct PositionBuffer {
    std::array<WirePosition, k_max_positions> positions{};
};

// `data` is a bare array here, not an object with a list inside it.
[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view body, PositionBuffer& storage);

// BingX's own documentation shows this answer both as a bare
// {"listenKey":".."} and wrapped in the usual envelope; both are read, because
// which one arrives decides whether the user stream connects at all.
[[nodiscard]] std::expected<std::string_view, WireError> parse_listen_key(std::string_view body);

// data.dualSidePosition, a STRING "true"/"false" rather than a JSON bool.
// True is hedge mode, where an order must name LONG or SHORT; false is one-way
// mode, where it must name BOTH and BingX refuses the others with 109400.
[[nodiscard]] std::expected<bool, WireError> parse_position_mode(std::string_view body);

// ------------------------------------------------------ user stream ----

enum class PrivateKind : std::uint8_t {
    OrderUpdate,      // ORDER_TRADE_UPDATE (perp) / spot.executionReport
    ListenKeyExpired, // the key died; the socket cannot recover without a new one
    Ack,              // a subscribe answer with code 0 (spot only; perp needs none)
    Error,            // the same shape with a non-zero code
    Ignorable,        // ACCOUNT_UPDATE / ACCOUNT_CONFIG_UPDATE: no sink channel
    Unknown,
};

[[nodiscard]] std::expected<PrivateKind, WireError> parse_private_event(std::string_view frame);

// The order push, letter-keyed on both trees. The PERPETUAL tree quotes every
// number and SPOT sends them bare, so each field is read as a raw scalar
// rather than as a string.
//
// `last_qty`/`last_price` are what a fill states; the coin-M copy of this push
// omits both and states only the cumulative, which is why a TRADE without them
// is dropped rather than reported at the average price. This adapter serves
// linear contracts, where BingX documents both.
struct WireOrderUpdate {
    std::string_view symbol;          // s
    std::string_view client_order_id; // c — absent on the spot push
    std::string_view order_id;        // i
    std::string_view side;            // S
    std::string_view type;            // o
    std::string_view exec_type;       // x: NEW/TRADE/CANCELED/EXPIRED/CALCULATED
    std::string_view price;           // p
    std::string_view orig_qty;        // q
    std::string_view last_qty;        // l
    std::string_view cum_qty;         // z
    std::string_view last_price;      // L
    std::string_view fee;             // n — negative when charged
    std::string_view trade_id;        // t — spot only; the perpetual push has none
    WireOrderStatus status{WireOrderStatus::Unknown}; // X
    bool is_maker{false};                             // m — spot only
    bool maker_known{false};
    std::uint64_t ts_ms{0}; // T
};

[[nodiscard]] std::expected<WireOrderUpdate, WireError>
parse_order_update(std::string_view frame);

} // namespace raikiri::venues::bingx
