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
    Depth,      // {sym}@depth5@100ms — a 5-level snapshot, not a delta
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

} // namespace raikiri::venues::bingx
